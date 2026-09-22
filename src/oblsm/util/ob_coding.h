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

#include "common/lang/string.h"

namespace oceanbase {
// 负责**内部 key /lookup_key 二进制编解码**
// LSM 的核心机制：**同一个 user key 多条版本，用 seq 区分新旧**，seq 越大版本越新。
static const uint8_t SEQ_SIZE               = 8;
static const uint8_t LOOKUP_KEY_PREFIX_SIZE = 8;

/**
 * @brief Appends a numeric value to a string in binary format.
 *
 * This template function takes a numeric value of any type and appends its binary
 * representation to the specified string.
 *
 * @tparam T The numeric type (e.g., `int`, `uint64_t`, `float`).
 * @param dst A pointer to the string to which the numeric value will be appended.
 * @param v The numeric value to append.
 */
// 把数值以原始二进制字节追加到string
// 直接取变量v的内存地址， 转成char*, 拷贝sizeof(T)个字节追加到dst字符串
template <typename T>
void put_numeric(string *dst, T v)
{
  dst->append(reinterpret_cast<char *>(&v), sizeof(T));
}

/**
 * @brief Extracts a numeric value from a binary data source.
 *
 * This template function reads a numeric value of any type from the provided
 * binary data source and returns it.
 *
 * @tparam T The numeric type to extract (e.g., `int`, `uint64_t`, `float`).
 * @param src A pointer to the source binary data from which the numeric value will be read.
 * @return The extracted numeric value of type `T`.
 */
// 从二进制内存src读取sizeof(T)字节，还原成数值T
template <typename T>
T get_numeric(const char *src)
{
  T value;
  memcpy(&value, src, sizeof(T));
  return value;
}

/**
 * @brief Extracts the user key portion from an internal key.
 *
 * An internal key in the LSM-Tree typically contains additional metadata such as
 * a sequence number at the end. This function removes the sequence number portion
 * and returns the user key portion.
 *
 * @param internal_key The internal key to extract the user key from.
 * @return A `string_view` representing the user key portion of the internal key.
 */
inline string_view extract_user_key(const string_view &internal_key)
{
  /*
┌───────────────┬──────────────┐
│   user_key    │  seq(8字节)  │
└───────────────┴──────────────┘
        ↑               ↑
   我们想要的        末尾8B序列号
[ user_key ] [ seq(8B, uint64_t) ]

*/
  return string_view(internal_key.data(), internal_key.size() - SEQ_SIZE);
}

/**
 * @brief Extracts the sequence number from an internal key.
 *
 * The sequence number is usually stored at the end of the internal key in
 * binary format. This function retrieves and returns the sequence number.
 *
 * @param internal_key The internal key to extract the sequence number from.
 * @return The extracted sequence number as a `uint64_t`.
 */
inline uint64_t extract_sequence(const string_view &internal_key)
{
  return get_numeric<uint64_t>(internal_key.data() + internal_key.size() - SEQ_SIZE);
}

/**
 * @brief Computes the size of the user key from a lookup key.
 *
 * A lookup key typically contains a prefix and a sequence number in addition
 * to the user key. This function calculates and returns the size of the user
 * key portion.
 *
 * @param lookup_key The lookup key to analyze.
 * @return The size of the user key portion in bytes.
 */
/*

┌────────────────────┬────────────┬────────────┐
│ LOOKUP_PREFIX(8B)  │ user_key   │ seq(8B)    │
└────────────────────┴────────────┴────────────┘
LOOKUP_KEY_PREFIX_SIZE=8        SEQ_SIZE=8

*/
inline size_t user_key_size_from_lookup_key(const string_view &lookup_key)
{
  return lookup_key.size() - SEQ_SIZE - LOOKUP_KEY_PREFIX_SIZE;
}

/**
 * @brief Extracts the user key from a lookup key.
 *
 * A lookup key in the LSM-Tree contains a prefix, user key, and sequence
 * number. This function extracts and returns the user key portion.
 *
 * @param lookup_key The lookup key to extract the user key from.
 * @return A `string_view` representing the user key portion of the lookup key.
 */
// 从 `lookup_key` 里面直接截取出来**user_key**（用户原始 key），返回一个零拷贝的 `string_view`。
inline string_view extract_user_key_from_lookup_key(const string_view &lookup_key)
{
  return string_view(lookup_key.data() + LOOKUP_KEY_PREFIX_SIZE, user_key_size_from_lookup_key(lookup_key));
}
// 输入 `lookup_key`，**跳过前面 8 字节 prefix，取出后面完整的 internal_key**（`user_key + seq(8B)`）。
inline string_view extract_internal_key(const string_view &lookup_key)
{
  return string_view(lookup_key.data() + LOOKUP_KEY_PREFIX_SIZE, lookup_key.size() - LOOKUP_KEY_PREFIX_SIZE);
}
// 解析**长度前缀字符串（length-prefixed string）**：二进制布局是【长度 +
// 字符串内容】，读出前面存的长度，跳过长度字段，返回字符串内容的 `string_view`。
inline string_view get_length_prefixed_string(const char *data)
{
  /*
┌──────────────────┬──────────────────────┐
│ size_t len(8B)   │ 字符串原始bytes       │
└──────────────────┴──────────────────────┘
↑data指针指向这里     ↑p指针指向这里

*/
  size_t      len = get_numeric<size_t>(data);
  const char *p   = data + sizeof(size_t);
  return string_view(p, len);
}

}  // namespace oceanbase