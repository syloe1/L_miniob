/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/table/ob_sstable_builder.h"
#include "oblsm/util/ob_coding.h"
#include "common/log/log.h"

namespace oceanbase {

// TODO: refactor build with mem_table/iterator logic.
RC ObSSTableBuilder::start_build(uint32_t sst_id, const std::string &file_name)
{
  // 清空上次构建残留状态，使 builder 可复用
  reset();
  sst_id_ = sst_id;

  // 创建文件写入器（覆盖写），失败返回 IO 打开错误
  file_writer_ = ObFileWriter::create_file_writer(file_name, false);
  if (file_writer_ == nullptr) {
    LOG_WARN("create sst file writer failed, file=%s", file_name.c_str());
    return RC::IOERR_OPEN;
  }
  return RC::SUCCESS;
}

RC ObSSTableBuilder::add(const string_view &key, const string_view &value)
{
  if (!has_pending_block_) {
    // 记录新 block 的 first key
    curr_blk_first_key_.assign(key.data(), key.size());
    has_pending_block_ = true;
  }

  RC rc = block_builder_.add(key, value);
  if (rc == RC::FULL) {
    // 当前块已满：先落盘，再把这 KV 写入新块
    finish_build_block();
    curr_blk_first_key_.assign(key.data(), key.size());
    rc = block_builder_.add(key, value);
  }
  return rc;
}

void ObSSTableBuilder::finish_build_table()
{
  // 最后一个块还有数据则落盘
  if (has_pending_block_) {
    finish_build_block();
    has_pending_block_ = false;
  }

  // 写 meta 区 + footer
  uint32_t meta_start = curr_offset_;
  uint32_t meta_count = static_cast<uint32_t>(block_metas_.size());

  {
    string buf;
    put_numeric<uint32_t>(&buf, meta_count);
    file_writer_->write(buf);
    curr_offset_ += buf.size();
  }

  for (const auto &meta : block_metas_) {
    string encoded = meta.encode();
    string buf;
    put_numeric<uint32_t>(&buf, static_cast<uint32_t>(encoded.size()));
    file_writer_->write(buf);
    curr_offset_ += buf.size();

    file_writer_->write(encoded);
    curr_offset_ += encoded.size();
  }

  {
    string buf;
    put_numeric<uint32_t>(&buf, meta_start);
    file_writer_->write(buf);
    curr_offset_ += buf.size();
  }

  file_size_ = curr_offset_;
  file_writer_->flush();
}

RC ObSSTableBuilder::build(shared_ptr<ObMemTable> mem_table, const std::string &file_name, uint32_t sst_id)
{
  RC rc = start_build(sst_id, file_name);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 遍历 memtable 内有序 KV，按块写入磁盘
  unique_ptr<ObLsmIterator> iter(mem_table->new_iterator());
  iter->seek_to_first();
  for (; iter->valid(); iter->next()) {
    rc = add(iter->key(), iter->value());
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }

  finish_build_table();
  return RC::SUCCESS;
}
// 当当前 block 写满 / MemTable 遍历结束时，调用这个函数：把 block_builder 里累积的 KV 打包成 block 二进制，写入 sst
// 文件
void ObSSTableBuilder::finish_build_block()
{
  string last_key = block_builder_.last_key();
  // 把积累的有序KV,编码成完整 block 二进制
  string_view block_contents = block_builder_.finish();
  file_writer_->write(block_contents);
  block_metas_.push_back(BlockMeta(curr_blk_first_key_, last_key, curr_offset_, block_contents.size()));
  // TODO: block aligned to BLOCK_SIZE
  // 更新文件偏移
  curr_offset_ += block_contents.size();
  block_builder_.reset();
}
// build把 memtable变成sst file,调用建一个ObSSTable对象
shared_ptr<ObSSTable> ObSSTableBuilder::get_built_table()
{
  // TODO: sstable should have more metadata
  shared_ptr<ObSSTable> sstable = make_shared<ObSSTable>(sst_id_, file_writer_->file_name(), comparator_, block_cache_);
  sstable->init();
  return sstable;
}
// 清空 ObSSTableBuilder 的所有状态，把 builder 恢复成初始干净状态，让这个 builder 对象可以复用，继续构建下一个 SST
// 文件。
void ObSSTableBuilder::reset()
{
  // 当前构建的data block
  block_builder_.reset();
  // 块的第一个key
  curr_blk_first_key_.clear();
  if (file_writer_ != nullptr) {
    file_writer_.reset(nullptr);
  }
  block_metas_.clear();
  curr_offset_ = 0;
  sst_id_      = 0;
  file_size_   = 0;
  has_pending_block_ = false;
}
}  // namespace oceanbase
