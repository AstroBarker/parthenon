//========================================================================================
// (C) (or copyright) 2020-2024. Triad National Security, LLC. All rights reserved.
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

#ifndef UTILS_TYPE_LIST_HPP_
#define UTILS_TYPE_LIST_HPP_

#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace parthenon {

// Convenience struct for holding a variadic pack of types
// and providing compile time indexing into that pack as
// well as the ability to get the index of a given type within
// the pack. Functions are available below for compile time
// concatenation of TypeLists
template <class... Args>
struct TypeList {
  using types = std::tuple<Args...>;

  static constexpr std::size_t n_types{sizeof...(Args)};

  template <std::size_t I>
  using type = typename std::tuple_element<I, types>::type;

  template <std::size_t... Idxs>
  using sublist = TypeList<type<Idxs>...>;

  template <class T, std::size_t I = 0>
  static constexpr std::size_t GetIdx() {
    static_assert(I < n_types, "Type is not present in TypeList.");
    if constexpr (std::is_same_v<T, type<I>>) {
      return I;
    } else {
      return GetIdx<T, I + 1>();
    }
  }

  template <class F>
  static void IterateTypes(F func) {
    (func(Args()), ...);
  }

 private:
  template <std::size_t Start, std::size_t End>
  static auto ContinuousSublist() {
    return ContinuousSublistImpl<Start>(std::make_index_sequence<End - Start + 1>());
  }
  template <std::size_t Start, std::size_t... Is>
  static auto ContinuousSublistImpl(std::index_sequence<Is...>) {
    return sublist<(Start + Is)...>();
  }

 public:
  template <std::size_t Start, std::size_t End>
  using continuous_sublist = decltype(ContinuousSublist<Start, End>());
};

namespace impl {
template <class... Args>
auto ConcatenateTypeLists(TypeList<Args...>) {
  return TypeList<Args...>();
}

template <class... Args1, class... Args2, class... Args>
auto ConcatenateTypeLists(TypeList<Args1...>, TypeList<Args2...>, Args...) {
  return ConcatenateTypeLists(TypeList<Args1..., Args2...>(), Args()...);
}
} // namespace impl

template <class... TLs>
using concatenate_type_lists_t =
    decltype(impl::ConcatenateTypeLists(std::declval<TLs>()...));

// Relevant only for lists of variable types
template <class TL>
auto GetNames() {
  std::vector<std::string> names;
  TL::IterateTypes([&names](auto t) { names.push_back(decltype(t)::name()); });
  return names;
}
} // namespace parthenon

#endif // UTILS_TYPE_LIST_HPP_
