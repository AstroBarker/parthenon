//========================================================================================
// (C) (or copyright) 2024. Triad National Security, LLC. All rights reserved.
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
#include <array>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "basic_types.hpp"
#include "defs.hpp"
#include "mesh/forest/forest_node.hpp"
#include "mesh/forest/forest_topology.hpp"
#include "mesh/forest/logical_location.hpp"
#include "mesh/forest/tree.hpp"
#include "utils/bit_hacks.hpp"
#include "utils/indexer.hpp"

namespace parthenon {
namespace forest {

template<class T> 
std::vector<std::shared_ptr<Node>> NodeListOverlap(T nodes_1, T nodes_2) {
  std::sort(std::begin(nodes_1), std::end(nodes_1));
  std::sort(std::begin(nodes_2), std::end(nodes_2));
  std::vector<std::shared_ptr<Node>> node_intersection;
  std::set_intersection(std::begin(nodes_1), std::end(nodes_1), 
                        std::begin(nodes_2), std::end(nodes_2), 
                        std::back_inserter(node_intersection));
  return node_intersection;
} 

void Face::SetNeighbors() { 
  std::unordered_set<std::shared_ptr<Face>> neighbors_local;
  for (auto &node : nodes)
    neighbors_local.insert(node->associated_faces.begin(),
                           node->associated_faces.end()); 
  for (std::shared_ptr<Face> neighbor : neighbors_local) { 
    auto node_overlap = NodeListOverlap(nodes, neighbor->nodes);
    if (node_overlap.size() > 0 && node_overlap.size() < 4) {
      std::array<int, 2> offset{0, 0};
      for (int i=0; i < 4; ++i) {
        if (std::find(node_overlap.begin(), node_overlap.end(), nodes[i]) != node_overlap.end()) {
          for (int o = 0; o < 2; ++o) offset[o] += static_cast<int>(node_to_offset[i][o]);
        }
      }
      for (auto &o : offset) o /= node_overlap.size();
      neighbors(offset[0], offset[1]).push_back(std::make_pair(neighbor, LogicalCoordinateTransformation()));
    }
  }
}

std::tuple<int, int, Offset> 
Face::GetEdgeDirections(const std::vector<std::shared_ptr<Node>> &nodes) {
  PARTHENON_REQUIRE(nodes.size() == 2, "The argument can't be an edge.");
  int I0 = face_index[nodes[0]];
  int I1 = face_index[nodes[1]];
  int dir_tang = (I1 - I0 > 0 ? 1 : -1) * IntegerLog2Floor(std::abs(I1 - I0));
  auto offsets = AverageOffsets(node_to_offset[I0], node_to_offset[I1]);
  PARTHENON_REQUIRE(offsets.IsEdge(), "Something is wrong.");
  
  Direction dir_norm;
  Offset offset;
  if (std::abs(static_cast<int>(offsets[0])) == 1) { 
    dir_norm = Direction::I;
    offset = offsets[0];
  } else if (std::abs(static_cast<int>(offsets[1])) == 1) {
    dir_norm = Direction::J; 
    offset = offsets[1];
  } else { 
    PARTHENON_FAIL("Shouldn't get here.");
  } 
  printf("I0 = %i I1 = %i dir_tan = %i dir_nrom = %i offsets=(%i, %i, %i)\n", 
      I0, I1, dir_tang, static_cast<int>(dir_norm), (int)(offsets[0]), (int)(offsets[1]), (int)(offsets[2]));
  return std::make_tuple(dir_tang, static_cast<int>(dir_norm), offset);
}


void Face::SetEdgeCoordinateTransforms() { 
  for (int ox = -1; ox <= 1; ++ox) {
    for (int oy = -1; oy <= 1; ++oy) {
      if (std::abs(ox) + std::abs(oy) == 1) { 
        for (auto &[neighbor, coord_trans] : neighbors(ox, oy)) { 
          auto node_overlap = NodeListOverlap(nodes, neighbor->nodes);
          PARTHENON_REQUIRE(node_overlap.size() == 2, "This is clearly not an edge.");
          std::sort(node_overlap.begin(), node_overlap.end(), [this](auto &n1, auto &n2){
              return this->face_index[n1] < this->face_index[n2];
            });
          
          auto [dir1, dir2, offset] = GetEdgeDirections(node_overlap);
          auto [dir1_neigh, dir2_neigh, offset_neigh] = neighbor->GetEdgeDirections(node_overlap);

          LogicalCoordinateTransformation ct; 
          // Set the direction mapping for direction along the edge
          ct.SetDirection(static_cast<Direction>(std::abs(dir1)), 
                          static_cast<Direction>(std::abs(dir1_neigh)), 
                          dir1_neigh < 0);
          // Set the direction mapping for direction tangent to the edge
          ct.SetDirection(static_cast<Direction>(std::abs(dir2)),
                          static_cast<Direction>(std::abs(dir2_neigh)),
                          offset == offset_neigh);
          coord_trans = ct;
        }
      }
    }
  }
}

void Face::SetNodeCoordinateTransforms() { 
  for (int ox = -1; ox <= 1; ++ox) {
    for (int oy = -1; oy <= 1; ++oy) {
      if (std::abs(ox) + std::abs(oy) == 2) { 
        for (auto &[neighbor, coord_trans] : neighbors(ox, oy)) { 
          // TODO(LFR): Find the shared edge neighbor  
        }
      }
    }
  }
}

} // namespace forest
} // namespace parthenon

