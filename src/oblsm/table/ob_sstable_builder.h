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

#include "common/lang/memory.h"
#include "oblsm/table/ob_block_builder.h"
#include "oblsm/memtable/ob_memtable.h"
#include "common/lang/string.h"
#include "oblsm/util/ob_file_writer.h"
#include "oblsm/table/ob_block.h"
#include "oblsm/table/ob_sstable.h"
#include "oblsm/util/ob_lru_cache.h"

namespace oceanbase {

/**
 * @brief Build a SSTable
 */
// 内存中的 ObMemTable 持久化为磁盘 SSTable 文件，负责分块、序列化、写文件、生成块元数据，
class ObSSTableBuilder
{
public:
  ObSSTableBuilder(const ObComparator *comparator, ObLRUCache<uint64_t, shared_ptr<ObBlock>> *block_cache)
      : comparator_(comparator), block_cache_(block_cache)
  {}
  ~ObSSTableBuilder() = default;

  /**
   * @brief Builds an SSTable from the provided in-memory table and stores it in a file.
   *
   * This function takes an `ObMemTable` as input, partitions the data into blocks,
   * serializes the blocks, and writes them into an SSTable file.
   *
   * @param mem_table A shared pointer to the `ObMemTable` containing the data to be written into the SSTable.
   * @param file_name The name of the file where the constructed SSTable will be stored.
   * @param sst_id A unique identifier assigned to the created SSTable.
   *
   * @return RC A result code indicating the success or failure of the SSTable creation process.
   *
   */
  // 接收memtable, 输出磁盘sst文件
  RC build(shared_ptr<ObMemTable> mem_table, const string &file_name, uint32_t sst_id);

  /**
   * @brief Starts building a new SSTable with the given id and file name.
   *
   * Resets any previous build state and opens a file writer for the new SSTable.
   *
   * @param sst_id The unique identifier assigned to the SSTable to be built.
   * @param file_name The path of the SSTable file.
   * @return RC indicating success or failure to open the file.
   */
  RC start_build(uint32_t sst_id, const string &file_name);

  /**
   * @brief Adds a single key-value pair to the SSTable being built.
   *
   * Handles block boundaries automatically: if the current block is full, it is
   * finalized and a new block is started.
   *
   * @param key The internal key to add.
   * @param value The value to add.
   * @return RC indicating success or failure.
   */
  RC add(const string_view &key, const string_view &value);

  /**
   * @brief Finalizes the current SSTable: flushes the pending block, writes the
   * block meta and footer, and flushes the file.
   */
  void finish_build_table();

  // 生成的 SSTable 文件总字节大小。
  size_t file_size() const { return file_size_; }
  // 当前正在构建的 SSTable 文件已写入的字节数。
  size_t curr_file_size() const { return curr_offset_; }
  // 构建完成后，创建并返回 ObSSTable 对象
  shared_ptr<ObSSTable> get_built_table();
  void                  reset();

private:
  // 结束当前Block的构建
  void finish_build_block();

  // 全局依赖
  const ObComparator                        *comparator_  = nullptr;  // Key 比较器
  ObLRUCache<uint64_t, shared_ptr<ObBlock>> *block_cache_ = nullptr;  // 块缓存

  // 块构建相关
  ObBlockBuilder block_builder_;       // 单个数据块构建器
  string         curr_blk_first_key_;  // 当前正在构建块的第一个Key

  // 文件写入与位置
  unique_ptr<ObFileWriter> file_writer_;      // 文件写入器，负责磁盘IO
  vector<BlockMeta>        block_metas_;      // 所有数据块的元数据（最终写入文件尾部）
  uint32_t                 curr_offset_ = 0;  // 文件当前写入偏移（字节）

  // 统计与标识
  uint32_t sst_id_    = 0;  // 待生成SSTable的唯一ID
  size_t   file_size_ = 0;  // 最终SSTable文件总大小
  bool     has_pending_block_ = false;  // 当前 block 是否已有待落盘数据
};
}  // namespace oceanbase
