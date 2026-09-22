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

#include "oblsm/util/ob_file_reader.h"
#include "common/lang/memory.h"
#include "common/sys/rc.h"
#include "oblsm/table/ob_block.h"
#include "oblsm/util/ob_comparator.h"
#include "oblsm/util/ob_lru_cache.h"

namespace oceanbase {
// TODO: add a dumptool to dump sst files(example for usage: ./dumptool sst_file)
//    ┌─────────────────┐
//    │    block 1      │◄──┐
//    ├─────────────────┤   │
//    │    block 2      │   │
//    ├─────────────────┤   │
//    │      ..         │   │
//    ├─────────────────┤   │
//    │    block n      │◄┐ │
//    ├─────────────────┤ │ │
// ┌─►│  meta size(n)   │ │ │
// │  ├─────────────────┤ │ │
// │  │block meta 1 size│ │ │
// │  ├─────────────────┤ │ │
// │  │  block meta 1   ┼─┼─┘
// │  ├─────────────────┤ │
// │  │      ..         │ │
// │  ├─────────────────┤ │
// │  │block meta n size│ │
// │  ├─────────────────┤ │
// │  │  block meta n   ┼─┘
// │  ├─────────────────┤
// └──┼                 │
//    └─────────────────┘

/**
 * @class ObSSTable
 * @brief Represents an SSTable (Sorted String Table) in the LSM-Tree.
 *
 * The `ObSSTable` class is responsible for managing on-disk sorted string tables (SSTables).
 * It provides methods for initialization, key-value lookups, block reading (with caching support),
 * and creating iterators for traversal. Each SSTable is uniquely identified by an `sst_id_` and
 * interacts with the LRU cache for efficient block access.
 */
// 继承 `enable_shared_from_this`：允许对象在持有`shared_ptr`时，调用`shared_from_this()`拿到自身 shared_ptr。
class ObSSTable : public enable_shared_from_this<ObSSTable>
{
public:
  /**
   * @brief Constructor for ObSSTable.
   *
   * Initializes an SSTable with its unique ID, file name, comparator, and block cache.
   *
   * @param sst_id A unique identifier for the SSTable.
   * @param file_name The name of the file storing the SSTable data.
   * @param comparator A pointer to the comparator used for key comparison.
   * @param block_cache A pointer to the LRU block cache for caching block-level data.
   */
  ObSSTable(uint32_t sst_id, const string &file_name, const ObComparator *comparator,
      ObLRUCache<uint64_t, shared_ptr<ObBlock>> *block_cache)
      : sst_id_(sst_id),
        file_name_(file_name),
        comparator_(comparator),
        file_reader_(nullptr),
        block_cache_(block_cache)  // 全局LRU块缓存
  {
    (void)block_cache_;
  }

  ~ObSSTable() = default;

  /**
   * @brief Initializes the SSTable instance.
   *
   * This function is responsible for performing setup tasks required for the SSTable,
   * such as preparing file readers or pre-loading block_metas_.
   *
   * @warning This function must be called before performing any operations on the SSTable.
   */
  // 任何SST操作前必须调用init()
  void init();
  // 获取文件唯一 ID；
  uint32_t sst_id() const { return sst_id_; }
  // 对外共享当前 SSTable 智能指针；
  shared_ptr<ObSSTable> get_shared_ptr() { return shared_from_this(); }

  ObLsmIterator *new_iterator();

  /**
   * @brief Reads a block from the SSTable using the block cache.
   *
   * Attempts to read the specified block using the block cache. If the block is not
   * in the cache, it will load the block from the SSTable file and update the cache.
   *
   * @param block_idx The index of the block to read.
   *
   * @return shared_ptr<ObBlock> A shared pointer to the requested block.
   */
  // 带缓存读取
  // 以 block_idx 为 Key 查询 LRU 块缓存；
  shared_ptr<ObBlock> read_block_with_cache(uint32_t block_idx) const;

  /**
   * @brief Reads a block directly from the SSTable file.
   *
   * This function bypasses the block cache and directly reads the requested block
   * from the SSTable file.
   *
   * @param block_idx The index of the block to read.
   *
   * @return shared_ptr<ObBlock> A shared pointer to the requested block.
   */
  // 直读磁盘（跳过缓存）
  shared_ptr<ObBlock> read_block(uint32_t block_idx) const;

  uint32_t block_count() const { return block_metas_.size(); }

  uint32_t size() const { return file_reader_->file_size(); }

  const BlockMeta block_meta(int i) const { return block_metas_[i]; }

  const ObComparator *comparator() const { return comparator_; }

  void   remove();
  string first_key() const { return block_metas_.empty() ? "" : block_metas_[0].first_key_; }
  string last_key() const { return block_metas_.empty() ? "" : block_metas_.back().last_key_; }

private:
  uint32_t                                   sst_id_;                // SSTable 唯一ID
  string                                     file_name_;             // 磁盘文件名
  const ObComparator                        *comparator_ = nullptr;  // Key 比较器
  unique_ptr<ObFileReader>                   file_reader_;           // 文件读取器，负责磁盘IO
  vector<BlockMeta>                          block_metas_;           // 所有数据块的元数据（常驻内存）
  ObLRUCache<uint64_t, shared_ptr<ObBlock>> *block_cache_;           // 全局块缓存
                                                                     // 全局块缓存
};
// 单个SSTable的迭代器
class TableIterator : public ObLsmIterator
{
public:
  TableIterator(const shared_ptr<ObSSTable> &sst) : sst_(sst), block_cnt_(sst->block_count()) {}
  ~TableIterator() override = default;

  void        seek(const string_view &key) override;
  void        seek_to_first() override;
  void        seek_to_last() override;
  void        next() override;
  bool        valid() const override { return block_iterator_ != nullptr && block_iterator_->valid(); }
  string_view key() const override { return block_iterator_->key(); }
  string_view value() const override { return block_iterator_->value(); }

private:
  void read_block_with_cache();

  const shared_ptr<ObSSTable> sst_;                 // 所属SSTable
  uint32_t                    block_cnt_;           // 总块数
  uint32_t                    curr_block_idx_ = 0;  // 当前遍历到的块下标
  shared_ptr<ObBlock>         block_;               // 当前加载的数据块
  unique_ptr<ObLsmIterator>   block_iterator_;      // 块内迭代器
};

using SSTablesPtr = shared_ptr<vector<vector<shared_ptr<ObSSTable>>>>;
/*
[
  [SST0, SST1, SST2],   // L0 层
  [SST3, SST4],         // L1 层
  [SST5, SST6, SST7]    // L2 层
  ...
]
*/
}  // namespace oceanbase
