/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/table/ob_block.h"
#include "oblsm/util/ob_coding.h"
#include "common/lang/memory.h"

namespace oceanbase {
//[entry1][entry2]...[entryN][offset_size(n)][offset1][offset2]...[offsetN][offset_start]

RC ObBlock::decode(const string &data)
{
  RC rc = RC::SUCCESS;
  // 指针指向的内存内容(*buf)不能改，指针buf本身可以修改
  const char    *buf             = data.data();
  uint32_t       block_total_len = static_cast<uint32_t>(data.size());
  const uint32_t uint32_len      = sizeof(uint32_t);
  // 最小长度：至少要能放下 offset_size(n) 和 offset start 两个 4 字节字段
  if (block_total_len < 2 * uint32_len) {
    rc = RC::INVALID_ARGUMENT;
    return rc;
  }
  // Step1：最后4字节 = offset start（也即 entry 区的字节长度）
  uint32_t offset_start = get_numeric<uint32_t>(buf + block_total_len - uint32_len);
  // offset start 之后至少要能放下 offset_size(n) 字段
  if (offset_start + uint32_len > block_total_len) {
    rc = RC::RANGE_ERROR;
    return rc;
  }
  // Step2：在 offset_start 处读取 offset_size(n) = entry 数量 N
  uint32_t offset_count       = get_numeric<uint32_t>(buf + offset_start);
  uint32_t offset_array_bytes = offset_count * uint32_len;
  // entry区 + count + offset数组 + 末尾 offset start 必须恰好填满，避免越界
  if (offset_start + uint32_len + offset_array_bytes + uint32_len != block_total_len) {
    rc = RC::RANGE_ERROR;
    return rc;
  }
  // Step3：entry区域 = [0, offset_start)，拷贝到data_，
  // 第二个参数是**字节个数**，不是结束地址。
  data_.assign(buf, offset_start);
  offsets_.resize(offset_count);
  // offset数组第一个offset起始地址：跳过 count 字段
  const char *offset_ptr = buf + offset_start + uint32_len;
  for (uint32_t i = 0; i < offset_count; i++) {
    offsets_[i] = get_numeric<uint32_t>(offset_ptr);
    offset_ptr += uint32_len;
  }

  return rc;
}
/*
SST里面有多个Block,block meta 用于存储 block 的元信息，包括 block 的大小、block 的位置信息，block 中的键值对数量等。
BlockIterator: Block内部迭代器， BlockMeta： SST索引里记录每个Block元信息
所有 entry 在前，offsets 数组在后，offsets 的起始位置存在 Block 末尾
*/
//**根据 entry 的下标 offset，从 Block 的 `data_` 里取出整条 entry 的只读视图（string_view），不拷贝内存**。
string_view ObBlock::get_entry(uint32_t offset) const
{
  // 区间是**左闭右开**，C++ 字符串 / 内存通用习惯 `[start, end)`
  uint32_t    curr_begin = offsets_[offset];
  uint32_t    curr_end   = offset == offsets_.size() - 1 ? data_.size() : offsets_[offset + 1];
  string_view curr       = string_view(data_.data() + curr_begin, curr_end - curr_begin);
  return curr;
}
//`offsets_` 数组里**一个元素对应一条 entry**，所以 `offsets_.size()` = entry 的条数。
ObLsmIterator *ObBlock::new_iterator() const { return new BlockIterator(comparator_, this, size()); }
//[ 4字节 uint32 key_len ] [ key内容 ] [ 4字节 uint32 val_len ] [ value内容 ]
//**取出这条 entry 原始二进制，拆出 key 和 value，更新迭代器内部的 curr_entry_ /key_/value_**。
void BlockIterator::parse_entry()
{
  curr_entry_         = data_->get_entry(index_);
  uint32_t key_size   = get_numeric<uint32_t>(curr_entry_.data());
  key_                = string_view(curr_entry_.data() + sizeof(uint32_t), key_size);
  uint32_t value_size = get_numeric<uint32_t>(curr_entry_.data() + sizeof(uint32_t) + key_size);
  value_              = string_view(curr_entry_.data() + 2 * sizeof(uint32_t) + key_size, value_size);
}
//  Marshal -> binary
string BlockMeta::encode() const
{
  //[4B first_key_len][first_key内容][4B last_key_len][last_key内容][4B offset][4B size]
  string ret;
  put_numeric<uint32_t>(&ret, first_key_.size());  // 第1步：写入 4B first_key_len
  ret.append(first_key_);                          // 第2步：写入 first_key 的原始字节
  put_numeric<uint32_t>(&ret, last_key_.size());   // 第3步：写入 4B last_key_len
  ret.append(last_key_);                           // 第4步：写入 last_key 的原始字节
  put_numeric<uint32_t>(&ret, offset_);            // 第5步：写入 4B offset
  put_numeric<uint32_t>(&ret, size_);              // 第6步：写入 4B size

  return ret;
}
// Unmarshal
RC BlockMeta::decode(const string &data)
{
  RC          rc       = RC::SUCCESS;   // ① 默认状态：成功
  const char *data_ptr = data.c_str();  // ② data_ptr 指向二进制buffer起始地址

  // ========== 读取 first_key ==========
  uint32_t first_key_size = get_numeric<uint32_t>(data_ptr);  // 读4字节：first_key长度
  data_ptr += sizeof(uint32_t);                               // 指针后移4字节，跳过长度字段
  first_key_.assign(data_ptr, first_key_size);                // 从data_ptr取first_key_size字节赋值给first_key_
  data_ptr += first_key_size;                                 // 指针跳过first_key变长内容

  // ========== 读取 last_key ==========
  uint32_t last_key_size = get_numeric<uint32_t>(data_ptr);  // 读4字节：last_key长度
  data_ptr += sizeof(uint32_t);
  last_key_.assign(data_ptr, last_key_size);
  data_ptr += last_key_size;

  // ========== 读取 offset_ 和 size_ ==========
  offset_ = get_numeric<uint32_t>(data_ptr);  // Block在SST中的偏移
  data_ptr += sizeof(uint32_t);
  size_ = get_numeric<uint32_t>(data_ptr);  // Block本身的字节大小
  return rc;
}

// 迭代器修改自身内部状态
void BlockIterator::seek(const string_view &lookup_key)
{
  index_ = 0;        // ① 重置下标，从头开始扫
  while (valid()) {  // ② 只要下标没越界（还有entry）就循环
    parse_entry();   // ③ 把当前 index_ 对应的entry解析成 key_, value_
    // ④ 取出用户key，做比较
    if (comparator_->compare(extract_user_key(key_), extract_user_key_from_lookup_key(lookup_key)) >= 0) {
      break;  // 找到第一条 >= lookup_key，停下
    }
    index_++;  // 当前这条key太小，继续下一条
  }
}

}  // namespace oceanbase
