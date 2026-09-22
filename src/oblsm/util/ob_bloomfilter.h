/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "common/lang/string.h"

namespace oceanbase {

/**
 * @class ObBloomfilter
 * @brief A simple Bloom filter implementation with support for concurrent access.
 *
 * A Bloom filter is a space-efficient probabilistic data structure used to test
 * whether an element is a member of a set. False positive matches are possible,
 * but false negatives are not — in other words, a query returns either
 * "possibly in set" or "definitely not in set".
 *
 * This implementation uses double hashing (Kirsch-Mitzenmacher optimization)
 * to generate k hash values from a single std::hash computation, and supports
 * concurrent reads via std::shared_mutex.
 */
class ObBloomfilter
{
public:
  /**
   * @brief Constructs a Bloom filter with specified parameters.
   *
   * @param hash_func_count Number of hash functions to use. Default is 4.
   * @param total_bits Total number of bits in the Bloom filter. Default is 65536.
   */
  ObBloomfilter(size_t hash_func_count = 4, size_t total_bits = 65536);

  /**
   * @brief Inserts an object into the Bloom filter.
   * @details This method computes hash values for the given object and sets
   *          corresponding bits in the filter. Thread-safe.
   * @param object The object to be inserted.
   */
  void insert(const string &object);

  /**
   * @brief Clears all entries in the Bloom filter.
   *
   * @details Resets the filter, removing all previously inserted objects. Thread-safe.
   */
  void clear();

  /**
   * @brief Checks if an object is possibly in the Bloom filter.
   *
   * @param object The object to be checked.
   * @return true if the object might be in the filter, false if definitely not.
   */
  bool contains(const string &object) const;

  /**
   * @brief Returns the count of objects inserted into the Bloom filter.
   */
  size_t object_count() const;

  /**
   * @brief Checks if the Bloom filter is empty.
   * @return true if the filter is empty, false otherwise.
   */
  bool empty() const { return 0 == object_count(); }

private:
  /**
   * @brief Computes the i-th hash value for the given object using double hashing.
   *
   * Uses the Kirsch-Mitzenmacher optimization: computes two independent hash
   * values from a single std::hash call, then derives the i-th hash as:
   *   h_i = (hash1 + i * hash2) % total_bits_
   *
   * @param object The object to hash.
   * @param i The index of the hash function (0-indexed).
   * @return The bit position for the i-th hash.
   */
  size_t hash(const string &object, size_t i) const;

  /// Number of hash functions used by the filter.
  size_t hash_func_count_;

  /// Total number of bits in the filter.
  size_t total_bits_;

  /// Number of objects currently inserted in the filter.
  size_t object_count_;

  /// Bit array backing the filter. Each byte stores 8 bits.
  std::vector<char> bits_;

  /// Mutex for concurrent access. Shared for reads (contains), exclusive for
  /// writes (insert, clear).
  mutable std::shared_mutex mutex_;
};

// =============================================================================
// Implementation
// =============================================================================

inline ObBloomfilter::ObBloomfilter(size_t hash_func_count, size_t total_bits)
    : hash_func_count_(hash_func_count), total_bits_(total_bits), object_count_(0)
{
  // Allocate enough bytes to hold total_bits_ bits (rounded up).
  // If total_bits_ is 0, the vector remains empty.
  if (total_bits_ > 0) {
    bits_.resize((total_bits_ + 7) / 8, 0);
  }
}

inline void ObBloomfilter::insert(const string &object)
{
  // 独占锁
  std::unique_lock<std::shared_mutex> lock(mutex_);

  if (total_bits_ == 0 || hash_func_count_ == 0) {
    return;
  }

  uint64_t hash_val = std::hash<string>{}(object);
  uint32_t hash1    = static_cast<uint32_t>(hash_val & 0xFFFFFFFF);
  uint32_t hash2    = static_cast<uint32_t>(hash_val >> 32);

  for (size_t i = 0; i < hash_func_count_; i++) {
    size_t pos = (hash1 + i * hash2) % total_bits_;
    bits_[pos / 8] |= static_cast<char>(1 << (pos % 8));
  }

  object_count_++;
}

inline void ObBloomfilter::clear()
{
  std::unique_lock<std::shared_mutex> lock(mutex_);

  std::fill(bits_.begin(), bits_.end(), 0);
  object_count_ = 0;
}

inline bool ObBloomfilter::contains(const string &object) const
{
  // 共享锁
  std::shared_lock<std::shared_mutex> lock(mutex_);

  if (total_bits_ == 0 || hash_func_count_ == 0) {
    return false;
  }

  uint64_t hash_val = std::hash<string>{}(object);
  uint32_t hash1    = static_cast<uint32_t>(hash_val & 0xFFFFFFFF);
  uint32_t hash2    = static_cast<uint32_t>(hash_val >> 32);

  for (size_t i = 0; i < hash_func_count_; i++) {
    size_t pos = (hash1 + i * hash2) % total_bits_;
    if (!(bits_[pos / 8] & (1 << (pos % 8)))) {
      return false;
    }
  }

  return true;
}

inline size_t ObBloomfilter::object_count() const
{
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return object_count_;
}

}  // namespace oceanbase
