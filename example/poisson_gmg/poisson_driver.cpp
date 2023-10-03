//========================================================================================
// (C) (or copyright) 2023. Triad National Security, LLC. All rights reserved.
//
// This program was produced under U.S. Government contract 89233218CNA000001 for Los
// Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
// for the U.S. Department of Energy/National Nuclear Security Administration. All rights
// in the program are reserved by Triad National Security, LLC, and the U.S. Department
// of Energy/National Nuclear Security Administration. The Government is granted for
// itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
// license in this material to reproduce, prepare derivative works, distribute copies to
// the public, perform publicly and display publicly, and to permit others to do so.
//========================================================================================

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// Local Includes
#include "amr_criteria/refinement_package.hpp"
#include "bvals/comms/bvals_in_one.hpp"
#include "interface/metadata.hpp"
#include "interface/update.hpp"
#include "mesh/meshblock_pack.hpp"
#include "parthenon/driver.hpp"
#include "poisson_driver.hpp"
#include "poisson_package.hpp"
#include "prolong_restrict/prolong_restrict.hpp"

using namespace parthenon::driver::prelude;

namespace poisson_example {

parthenon::DriverStatus PoissonDriver::Execute() {
  pouts->MakeOutputs(pmesh, pinput);
  ConstructAndExecuteTaskLists<>(this);
  pouts->MakeOutputs(pmesh, pinput);
  return DriverStatus::complete;
}

TaskCollection PoissonDriver::MakeTaskCollection(BlockList_t &blocks) {
  auto pkg = pmesh->packages.Get("poisson_package");
  auto solver = pkg->Param<std::string>("solver");
  if (solver == "BiCGSTAB") {
    return MakeTaskCollectionMGBiCGSTAB(blocks);
  } else if (solver == "MG") {
    return MakeTaskCollectionMG(blocks);
  } else {
    PARTHENON_FAIL("Unknown solver type.");
  }
  return TaskCollection();
}

template <parthenon::BoundaryType comm_boundary, class in_t, class out_t, class TL_t>
TaskID AddJacobiIteration(TL_t &tl, TaskID depends_on, bool multilevel, Real omega,
                          std::shared_ptr<MeshData<Real>> &md) {
  using namespace parthenon;
  using namespace poisson_package;
  TaskID none(0);

  auto comm = AddBoundaryExchangeTasks<comm_boundary>(depends_on, tl, md, multilevel);
  auto flux = tl.AddTask(comm, CalculateFluxes<in_t, true>, md);
  auto mat_mult = tl.AddTask(flux, FluxMultiplyMatrix<in_t, out_t, true>, md, false);
  return tl.AddTask(mat_mult, FluxJacobi<out_t, in_t, out_t, true>, md, omega,
                    GSType::all);
}

template <parthenon::BoundaryType comm_boundary, class TL_t>
TaskID AddSRJIteration(TL_t &tl, TaskID depends_on, int stages, bool multilevel,
                       std::shared_ptr<MeshData<Real>> &md) {
  using namespace parthenon;
  using namespace poisson_package;
  int ndim = md->GetParentPointer()->ndim;

  std::array<std::array<Real, 3>, 3> omega_M1{
      {{1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}};
  // Damping factors from Yang & Mittal (2017)
  std::array<std::array<Real, 3>, 3> omega_M2{
      {{0.8723, 0.5395, 0.0000}, {1.3895, 0.5617, 0.0000}, {1.7319, 0.5695, 0.0000}}};
  std::array<std::array<Real, 3>, 3> omega_M3{
      {{0.9372, 0.6667, 0.5173}, {1.6653, 0.8000, 0.5264}, {2.2473, 0.8571, 0.5296}}};

  auto omega = omega_M1;
  if (stages == 2) omega = omega_M2;
  if (stages == 3) omega = omega_M3;
  // This copy is to set the boundaries of temp that will not be updated by boundary
  // communication to the values in u
  depends_on = tl.AddTask(depends_on, CopyData<u, temp>, md);
  auto jacobi1 = AddJacobiIteration<comm_boundary, u, temp>(tl, depends_on, multilevel,
                                                            omega[ndim - 1][0], md);
  if (stages < 2) {
    return tl.AddTask(jacobi1, CopyData<temp, u>, md);
  }
  auto jacobi2 = AddJacobiIteration<comm_boundary, temp, u>(tl, jacobi1, multilevel,
                                                            omega[ndim - 1][1], md);
  if (stages < 3) return jacobi2;
  auto jacobi3 = AddJacobiIteration<comm_boundary, u, temp>(tl, jacobi2, multilevel,
                                                            omega[ndim - 1][2], md);
  return tl.AddTask(jacobi3, CopyData<temp, u>, md);
}

template <class x_t, class y_t, class out_t, bool only_md_level = false, class TL_t>
TaskID Axpy(TL_t &tl, TaskID depends_on, std::shared_ptr<MeshData<Real>> &md,
            Real weight_Ax, Real weight_y, bool only_interior, bool do_flux_cor = false) {
  using namespace parthenon;
  using namespace poisson_package;
  auto flux_res = tl.AddTask(depends_on, CalculateFluxes<x_t, only_md_level>, md);
  if (do_flux_cor && !only_md_level) {
    auto start_flxcor = tl.AddTask(flux_res, StartReceiveFluxCorrections, md);
    auto send_flxcor = tl.AddTask(flux_res, LoadAndSendFluxCorrections, md);
    auto recv_flxcor = tl.AddTask(send_flxcor, ReceiveFluxCorrections, md);
    flux_res = tl.AddTask(recv_flxcor, SetFluxCorrections, md);
  }
  auto Ax_res = tl.AddTask(flux_res, FluxMultiplyMatrix<x_t, temp, only_md_level>, md,
                           only_interior);
  return tl.AddTask(Ax_res,
                    AddFieldsAndStoreInteriorSelect<temp, y_t, out_t, only_md_level>, md,
                    weight_Ax, weight_y, only_interior);
}

template <class a_t, class b_t, class TL_t>
TaskID DotProduct(TaskID dependency_in, TaskRegion &region, TL_t &tl, int partition,
                  int &reg_dep_id, AllReduce<Real> *adotb,
                  std::shared_ptr<MeshData<Real>> &md) {
  using namespace parthenon;
  using namespace poisson_package;
  auto zero_adotb = (partition == 0 ? tl.AddTask(
                                          dependency_in,
                                          [](AllReduce<Real> *r) {
                                            r->val = 0.0;
                                            return TaskStatus::complete;
                                          },
                                          adotb)
                                    : dependency_in);
  region.AddRegionalDependencies(reg_dep_id, partition, zero_adotb);
  reg_dep_id++;
  auto get_adotb = tl.AddTask(zero_adotb, DotProductLocal<a_t, b_t>, md, &(adotb->val));
  region.AddRegionalDependencies(reg_dep_id, partition, get_adotb);
  reg_dep_id++;
  auto start_global_adotb =
      (partition == 0
           ? tl.AddTask(get_adotb, &AllReduce<Real>::StartReduce, adotb, MPI_SUM)
           : get_adotb);
  auto finish_global_adotb =
      tl.AddTask(start_global_adotb, &AllReduce<Real>::CheckReduce, adotb);
  region.AddRegionalDependencies(reg_dep_id, partition, finish_global_adotb);
  reg_dep_id++;
  return finish_global_adotb;
}

template <class TL_t>
TaskID PoissonDriver::AddMultiGridTasksPartitionLevel(TL_t &tl, TaskID dependence, int partition, int level, int min_level,
                                           int max_level, bool final) {
  using namespace parthenon;
  using namespace poisson_package;
  TaskID none(0);
  const int num_partitions = pmesh->DefaultNumPartitions();

  auto pkg = pmesh->packages.Get("poisson_package");
  auto damping = pkg->Param<Real>("jacobi_damping");
  auto smoother = pkg->Param<std::string>("smoother");
  int pre_stages = pkg->Param<int>("pre_smooth_iterations");
  int post_stages = pkg->Param<int>("post_smooth_iterations");
  bool do_FAS = pkg->Param<bool>("do_FAS");
  if (smoother == "SRJ1") {
    pre_stages = 1;
    post_stages = 1;
  } else if (smoother == "SRJ2") {
    pre_stages = 2;
    post_stages = 2;
  } else if (smoother == "SRJ3") {
    pre_stages = 3;
    post_stages = 3;
  } else {
    PARTHENON_FAIL("Unknown solver type.");
  }

  int ndim = pmesh->ndim;
  bool multilevel = (level != min_level);
  TaskID last_task;
  
  auto &md = pmesh->gmg_mesh_data[level].GetOrAdd(level, "base", partition);
  // 0. Receive residual from coarser level if there is one
  auto set_from_finer = dependence;
  if (level < max_level) {
    // Fill fields with restricted values
    auto recv_from_finer =
        tl.AddTask(dependence, ReceiveBoundBufs<BoundaryType::gmg_restrict_recv>, md);
    set_from_finer =
        tl.AddTask(recv_from_finer, SetBounds<BoundaryType::gmg_restrict_recv>, md);
    // 1. Copy residual from dual purpose communication field to the rhs, should be
    // actual RHS for finest level
    auto copy_u = tl.AddTask(set_from_finer, CopyData<u, u0, true>, md);
    if (!do_FAS) {
      auto zero_u = tl.AddTask(copy_u, SetToZero<u, true>, md);
      auto copy_rhs = tl.AddTask(set_from_finer, CopyData<res_err, rhs, true>, md);
      set_from_finer = zero_u | copy_u | copy_rhs;
    } else {
      set_from_finer = AddBoundaryExchangeTasks<BoundaryType::gmg_same>(
          set_from_finer, tl, md, multilevel);
      // This should set the rhs only in blocks that correspond to interior nodes, the
      // RHS of leaf blocks that are on this GMG level should have already been set on
      // entry into multigrid
      set_from_finer =
          Axpy<u, res_err, rhs, true>(tl, set_from_finer, md, 1.0, 1.0, true);
      set_from_finer = set_from_finer | copy_u;
    }
  } else {
    set_from_finer = tl.AddTask(set_from_finer, CopyData<u, u0, true>, md);
  }

  // 2. Do pre-smooth and fill solution on this level
  auto pre_smooth = AddSRJIteration<BoundaryType::gmg_same>(tl, set_from_finer,
                                                            pre_stages, multilevel, md);
  // If we are finer than the coarsest level:
  auto post_smooth = none;
  if (level > min_level) {
    // 3. Communicate same level boundaries so that u is up to date everywhere
    auto comm_u = AddBoundaryExchangeTasks<BoundaryType::gmg_same>(pre_smooth, tl, md,
                                                                   multilevel);

    // 4. Caclulate residual and store in communication field
    auto residual = Axpy<u, rhs, res_err, true>(tl, comm_u, md, -1.0, 1.0, false);

    // 5. Restrict communication field and send to next level
    auto communicate_to_coarse =
        tl.AddTask(residual, SendBoundBufs<BoundaryType::gmg_restrict_send>, md);

    // 6. Receive error field into communication field and prolongate
    auto recv_from_coarser = tl.AddTask(
        communicate_to_coarse, ReceiveBoundBufs<BoundaryType::gmg_prolongate_recv>, md);
    auto set_from_coarser =
        tl.AddTask(recv_from_coarser, SetBounds<BoundaryType::gmg_prolongate_recv>, md);
    auto prolongate = tl.AddTask(
        set_from_coarser, ProlongateBounds<BoundaryType::gmg_prolongate_recv>, md);

    // 7. Correct solution on this level with res_err field and store in
    //    communication field
    auto update_sol =
        tl.AddTask(prolongate, AddFieldsAndStore<u, res_err, u, true>, md, 1.0, 1.0);

    // 8. Post smooth using communication field and stored RHS
    post_smooth = AddSRJIteration<BoundaryType::gmg_same>(tl, update_sol, post_stages,
                                                          multilevel, md);
  } else {
    post_smooth = tl.AddTask(pre_smooth, CopyData<u, res_err, true>, md);
  }

  // 9. Send communication field to next finer level (should be error field for that
  // level)
  if (level < max_level) {
    auto copy_over = post_smooth;
    if (!do_FAS) {
      copy_over = tl.AddTask(post_smooth, CopyData<u, res_err, true>, md);
    } else {
      auto calc_err = tl.AddTask(post_smooth, AddFieldsAndStore<u, u0, res_err, true>,
                                 md, 1.0, -1.0);
      copy_over = calc_err;
    }
    auto boundary =
        AddBoundaryExchangeTasks<BoundaryType::gmg_same>(copy_over, tl, md, multilevel);
    last_task = tl.AddTask(boundary, SendBoundBufs<BoundaryType::gmg_prolongate_send>, md);
  } else {
    last_task = AddBoundaryExchangeTasks<BoundaryType::gmg_same>(post_smooth, tl, md, multilevel);
  }
  return last_task;
}

TaskCollection PoissonDriver::MakeTaskCollectionMG(BlockList_t &blocks) {
  using namespace parthenon;
  using namespace poisson_package;
  TaskCollection tc;
  TaskID none(0);

  auto pkg = pmesh->packages.Get("poisson_package");
  auto max_iterations = pkg->Param<int>("max_iterations");
  auto residual_tolerance = pkg->Param<Real>("residual_tolerance");
  this->mg_iter_cntr = 0; 

  const int num_partitions = pmesh->DefaultNumPartitions();
  int min_level = 0;
  int max_level = pmesh->GetGMGMaxLevel();

  TaskRegion &region = tc.AddRegion(num_partitions);
  
  std::vector<IterativeTasks> iter_tls(num_partitions); 
  for (int i = 0; i < num_partitions; ++i) { 
    iter_tls[i] = region[i].AddIteration("MG");
  }

  int reg_dep_id = 0;
  for (int i = 0; i < num_partitions; ++i) {
    auto &iter_tl = iter_tls[i];
    TaskList &tl = region[i];

    auto &md = pmesh->mesh_data.GetOrAdd("base", i);
    auto copy_exact = tl.AddTask(none, CopyData<exact, u>, md);
    auto comm = AddBoundaryExchangeTasks<BoundaryType::any>(copy_exact, tl, md, true);
    auto get_rhs = Axpy<u, u, rhs>(tl, comm, md, 1.0, 0.0, false, false);
    auto zero_u = tl.AddTask(none, SetToZero<u>, md);
    if (i == 0) {
      tl.AddTask(
          zero_u,
          [&]() {
            printf("# [0] v-cycle\n# [1] rms-residual\n# [2] rms-error\n");
            return TaskStatus::complete;
          });
    } 

    for (int level = max_level - 1; level >= min_level; --level)
      AddMultiGridTasksPartitionLevel(iter_tl, none, i, level, min_level, max_level, level == min_level); 
    auto mg_finest = AddMultiGridTasksPartitionLevel(iter_tl, none, i, max_level, min_level, max_level, false);
    
    auto calc_pointwise_res = Axpy<u, rhs, res_err>(iter_tl, mg_finest, md, -1.0, 1.0, false);
    auto get_res = DotProduct<res_err, res_err>(calc_pointwise_res, region, iter_tl, i,
                                                reg_dep_id, &residual, md);
    auto calc_err =
        iter_tl.AddTask(get_res, AddFieldsAndStore<u, exact, res_err>, md, 1.0, -1.0);
    auto get_err = DotProduct<res_err, res_err>(calc_err, region, iter_tl, i, reg_dep_id,
                                                &rhat0r, md);

    auto bound_exch = AddBoundaryExchangeTasks<BoundaryType::any>(get_res, iter_tl, md, true);
    auto check = iter_tl.SetCompletionTask(bound_exch | get_err, [&](PoissonDriver *driver, int partition, int max_iter, Real res_tol){
      if (partition != 0) TaskStatus::complete; 
      this->mg_iter_cntr++;
      Real rms_res = std::sqrt(driver->residual.val / pmesh->GetTotalCells());
      Real rms_err = std::sqrt(driver->rhat0r.val / pmesh->GetTotalCells());
      printf("%i %e %e\n", mg_iter_cntr, rms_res, rms_err); 
      if (rms_res > res_tol && this->mg_iter_cntr < max_iter) return TaskStatus::iterate;
      return TaskStatus::complete;
      }, this, i, max_iterations, residual_tolerance);
    region.AddGlobalDependencies(reg_dep_id, i, check);
  }

  return tc;
}

TaskCollection PoissonDriver::MakeTaskCollectionMGBiCGSTAB(BlockList_t &blocks) {
  using namespace parthenon;
  using namespace poisson_package;
  TaskCollection tc;
  TaskID none(0);

  auto pkg = pmesh->packages.Get("poisson_package");
  auto max_iterations = pkg->Param<int>("max_iterations");
  auto precondition = pkg->Param<bool>("precondition");
  bool flux_correct = pkg->Param<bool>("flux_correct");
  const int num_partitions = pmesh->DefaultNumPartitions();
  int n_vcycles = pkg->Param<int>("precondition_vcycles");

  Real restart_threshold = pkg->Param<Real>("restart_threshold");

  int min_level = 0;
  int max_level = pmesh->GetGMGMaxLevel();
  this->mg_iter_cntr = 0;
  auto AddGMGRegion = [&](IterativeTasks &itl, TaskID dependency, int partition) {
    if (precondition) {
      for (int level = max_level - 1; level >= min_level; --level) {
        AddMultiGridTasksPartitionLevel(itl, none, partition, level, min_level, max_level, level == min_level);
      }
      return AddMultiGridTasksPartitionLevel(itl, dependency, partition, max_level, min_level, max_level, max_level == min_level);
    } else {
      auto &md = pmesh->mesh_data.GetOrAdd("base", partition);
      return itl.AddTask(dependency, CopyData<rhs, u>, md);
    }
  };

  // Solving A x = rhs with BiCGSTAB possibly with pre-conditioner M^{-1} such that A M ~
  // I Initialization: x <- 0, r <- rhs, rhat0 <- rhs, rhat0r_old <- (rhat0, r), p <- r, u
  // <- 0
  {
    int reg_dep_id = 0;
    TaskRegion &region = tc.AddRegion(num_partitions);
    std::vector<IterativeTasks> iter_tls(num_partitions); 

    for (int i = 0; i < num_partitions; ++i) {
      TaskList &tl = region[i];
      auto &md = pmesh->mesh_data.GetOrAdd("base", i);
      auto zero_x = tl.AddTask(none, SetToZero<x>, md);
      auto zero_u = tl.AddTask(none, SetToZero<u>, md);
      auto copy_r = tl.AddTask(none, CopyData<rhs, r>, md);
      auto copy_p = tl.AddTask(none, CopyData<rhs, p>, md);
      auto copy_rhsbase = tl.AddTask(none, CopyData<rhs, rhs_base>, md);
      auto copy_rhat0 = tl.AddTask(none, CopyData<rhs, rhat0>, md);
      auto get_rhat0r =
          DotProduct<rhat0, r>(none, region, tl, i, reg_dep_id, &rhat0r, md);
      if (i == 0) {
        tl.AddTask(
            get_rhat0r,
            [this](PoissonDriver *driver) {
              driver->rhat0r_old = driver->rhat0r.val;
              driver->rhat0r.val = 0.0;
              driver->rhat0v.val = 0.0;
              driver->ts.val = 0.0;
              driver->tt.val = 0.0;
              driver->residual.val = 0.0;
              return TaskStatus::complete;
            },
            this);
      }
      if (i == 0) {
        tl.AddTask(
            none,
            [&]() {
              printf("# [0] v-cycle\n# [1] rms-residual\n# [2] rms-error\n");
              return TaskStatus::complete;
            });
      }
    }
  }
  {
    int ivcycle = 0;
    TaskRegion &region = tc.AddRegion(num_partitions);
    std::vector<IterativeTasks> iter_tls(num_partitions); 
    for (int i = 0; i < num_partitions; ++i) { 
      iter_tls[i] = region[i].AddIteration("MG");
    }
    int reg_dep_id = 0;
    for (int i = 0; i < num_partitions; ++i) {
      //TaskList &tl = region[i];
      auto &itl = iter_tls[i];
      auto &md = pmesh->mesh_data.GetOrAdd("base", i);
      
      // 1. u <- M p (rhs = p is set in previous cycle or initialization)
      auto precon1 = AddGMGRegion(itl, none, i);

      // 2. v <- A u
      auto comm = AddBoundaryExchangeTasks<BoundaryType::any>(precon1, itl, md, true);
      auto get_v = Axpy<u, u, v>(itl, comm, md, 1.0, 0.0, false, flux_correct);

      // 3. rhat0v <- (rhat0, v)
      auto get_rhat0v =
          DotProduct<rhat0, v>(get_v, region, itl, i, reg_dep_id, &rhat0v, md);

      // 4. h <- x + alpha u (alpha = rhat0r_old / rhat0v)
      auto correct_h = itl.AddTask(
          get_rhat0v,
          [](PoissonDriver *driver, std::shared_ptr<MeshData<Real>> &md) {
            Real alpha = driver->rhat0r_old / driver->rhat0v.val;
            return AddFieldsAndStore<x, u, h>(md, 1.0, alpha);
          },
          this, md);

      // 5. s <- r - alpha v (alpha = rhat0r_old / rhat0v)
      auto correct_s = itl.AddTask(
          get_rhat0v,
          [](PoissonDriver *driver, std::shared_ptr<MeshData<Real>> &md) {
            Real alpha = driver->rhat0r_old / driver->rhat0v.val;
            return AddFieldsAndStore<r, v, s>(md, 1.0, -alpha);
          },
          this, md);

      // Check and print out residual
      auto get_res = DotProduct<s, s>(correct_s, region, itl, i, reg_dep_id,
                                      &residual, md);

      region.AddRegionalDependencies(reg_dep_id, i, get_res);
      if (i == 0) {
        itl.AddTask(
            get_res,
            [&](PoissonDriver *driver, int iter) {
              Real rms_err = std::sqrt(driver->residual.val / pmesh->GetTotalCells());
              printf("%i %e\n", iter, rms_err);
              return TaskStatus::complete;
            },
            this, ivcycle * 2 + 1);
      }
      // Setup for MG Precondition
      auto set_rhs = itl.AddTask(correct_s, CopyData<s, rhs>, md);
      auto zero_u = itl.AddTask(correct_s, SetToZero<u>, md);

      // 6. u <- M s (rhs = s)
      auto precon2 = AddGMGRegion(itl, set_rhs | zero_u, i);

      // 7. t <- A u
      auto pre_t_comm = AddBoundaryExchangeTasks<BoundaryType::any>(precon2, itl, md, true);
      auto get_t = Axpy<u, u, t>(itl, pre_t_comm, md, 1.0, 0.0, false, flux_correct);

      // 8. omega <- (t,s) / (t,t)
      auto get_ts = DotProduct<t, s>(get_t, region, itl, i, reg_dep_id, &ts, md);
      auto get_tt = DotProduct<t, t>(get_t, region, itl, i, reg_dep_id, &tt, md);

      // 9. x <- h + omega u
      auto correct_x = itl.AddTask(
          get_tt | get_ts,
          [](PoissonDriver *driver, std::shared_ptr<MeshData<Real>> &md) {
            Real omega = driver->ts.val / driver->tt.val;
            return AddFieldsAndStore<h, u, x>(md, 1.0, omega);
          },
          this, md);

      // 10. r <- s - omega t
      auto correct_r = itl.AddTask(
          get_tt | get_ts,
          [](PoissonDriver *driver, std::shared_ptr<MeshData<Real>> &md) {
            Real omega = driver->ts.val / driver->tt.val;
            return AddFieldsAndStore<s, t, r>(md, 1.0, -omega);
          },
          this, md);
      
      // Check and print out residual
      auto get_res2 = DotProduct<r, r>(correct_r, region, itl, i, reg_dep_id,
                                      &residual, md);

      region.AddRegionalDependencies(reg_dep_id, i, get_res2);
      if (i == 0) {
        get_res2 = itl.AddTask(
            get_res2,
            [&](PoissonDriver *driver, int iter) {
              Real rms_err = std::sqrt(driver->residual.val / pmesh->GetTotalCells());
              printf("%i %e\n", iter, rms_err);
              return TaskStatus::complete;
            },
            this, ivcycle * 2 + 2);
      }

      // 10.5. Restart if (rhat0, r) is below threshold
      region.AddRegionalDependencies(reg_dep_id, i, correct_r | correct_x | get_res2);
      reg_dep_id++;
      auto restart = itl.AddTask(
          correct_r,
          [restart_threshold](PoissonDriver *driver, int i,
                              std::shared_ptr<MeshData<Real>> &md) {
            if (i == 0 && std::abs(driver->rhat0r_old) < restart_threshold)
              printf("Restart rhat0r_old = %e (%e)\n", driver->rhat0r_old,
                     restart_threshold);
            if (std::abs(driver->rhat0r_old) < restart_threshold) {
              CopyData<r, rhat0>(md);
              CopyData<r, p>(md);
            }
            return TaskStatus::complete;
          },
          this, i, md);

      // 11. rhat0r <- (rhat0, r)
      auto get_rhat0r =
          DotProduct<rhat0, r>(correct_r, region, itl, i, reg_dep_id, &rhat0r, md);

      // 12. beta <- rhat0r / rhat0r_old * alpha / omega
      // 13. p <- r + beta * (p - omega * v)
      auto update_p = itl.AddTask(
          get_rhat0r,
          [restart_threshold](PoissonDriver *driver,
                              std::shared_ptr<MeshData<Real>> &md) {
            if (std::abs(driver->rhat0r_old) >= restart_threshold) {
              Real alpha = driver->rhat0r_old / driver->rhat0v.val;
              Real omega = driver->ts.val / driver->tt.val;
              Real beta = driver->rhat0r.val / driver->rhat0r_old * alpha / omega;
              AddFieldsAndStore<p, v, p>(md, 1.0, -omega);
              return AddFieldsAndStore<r, p, p>(md, 1.0, beta);
            }
            return TaskStatus::complete;
          },
          this, md);

      // Set up for MG in next cycle
      itl.AddTask(update_p, CopyData<p, rhs>, md);
      itl.AddTask(update_p, SetToZero<u>, md);
      
      // 14. rhat0r_old <- rhat0r, zero all reductions
      region.AddRegionalDependencies(reg_dep_id, i, update_p | correct_x);
      auto check = itl.SetCompletionTask(
          update_p | correct_x,
          [](PoissonDriver *driver, int partition) {
            if (partition != 0) return TaskStatus::complete;
            
            if (driver->residual.val < 1.e-12 || driver->mg_iter_cntr > 10) return TaskStatus::complete;
            driver->mg_iter_cntr++;
            driver->rhat0r_old = driver->rhat0r.val;
            driver->rhat0r.val = 0.0;
            driver->rhat0v.val = 0.0;
            driver->ts.val = 0.0;
            driver->tt.val = 0.0;
            driver->residual.val = 0.0;
            return TaskStatus::iterate;
          },
          this, i);
      region.AddGlobalDependencies(reg_dep_id, i, check);
    }
  }
  

  return tc;
}

} // namespace poisson_example
