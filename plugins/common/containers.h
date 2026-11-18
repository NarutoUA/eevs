#pragma once

/**
 * @file containers.h
 * @brief Internal plugin helper for flat associative containers with fallback for older libstdc++.
 */

#include <functional>

#if __has_include(<flat_map>)
#include <flat_map>
#else
#include <map>
#endif

#if __has_include(<flat_set>)
#include <flat_set>
#else
#include <set>
#endif

namespace eevs {

#if defined(__cpp_lib_flat_map) && __cpp_lib_flat_map >= 202207L
template <typename Key, typename Tp, typename Compare = std::less<Key>>
using FlatMap = std::flat_map<Key, Tp, Compare>;
#else
template <typename Key, typename Tp, typename Compare = std::less<Key>>
using FlatMap = std::map<Key, Tp, Compare>;
#endif

#if defined(__cpp_lib_flat_set) && __cpp_lib_flat_set >= 202207L
template <typename Key, typename Compare = std::less<Key>>
using FlatSet = std::flat_set<Key, Compare>;

template <typename Key, typename Compare = std::less<Key>>
using FlatMultiset = std::flat_multiset<Key, Compare>;
#else
template <typename Key, typename Compare = std::less<Key>>
using FlatSet = std::set<Key, Compare>;

template <typename Key, typename Compare = std::less<Key>>
using FlatMultiset = std::multiset<Key, Compare>;
#endif

} // namespace eevs
