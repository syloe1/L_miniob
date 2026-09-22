/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/table/ob_sstable.h"
#include "oblsm/util/ob_coding.h"
#include "common/log/log.h"
#include "common/lang/filesystem.h"
#include "oblsm/util/ob_file_reader.h"
namespace oceanbase {

void ObSSTable::init()
{
  //**从 SST 文件尾部反向读取 meta 元数据，加载所有 BlockMeta 到内存`block_metas_`数组**。
  // 读文件尾部， 读出BlockMeta列表， 填充到block_metas
  // create_file_reader 内部会 open 文件，失败返回 nullptr
  file_reader_ = ObFileReader::create_file_reader(file_name_);
  if (file_reader_ == nullptr) {
    LOG_ERROR("open sst file failed, file=%s", file_name_.c_str());
    return;
  }
  // 获取整个文件大小；**最小合法性校验**：SST 至少要有末尾 4 字节 footer，否则是损坏空文件。
  uint32_t file_size = file_reader_->file_size();
  // 文件至少要能放下 4 字节的 footer（meta 区偏移）
  if (file_size < sizeof(uint32_t)) {
    LOG_ERROR("sst file is too small, file=%s", file_name_.c_str());
    return;
  }

  // 文件末尾 4 字节 = meta 区起始偏移（即 "meta size(n)" 所在位置）
  string footer = file_reader_->read_pos(file_size - sizeof(uint32_t), sizeof(uint32_t));
  // footer存meta_start： meta区域在整个SST里的起始位置偏移
  if (footer.size() != sizeof(uint32_t)) {
    LOG_ERROR("read sst footer failed, file=%s", file_name_.c_str());
    return;
  }
  uint32_t meta_start = get_numeric<uint32_t>(footer.data());

  // meta 区头部 4 字节 = block 数量 meta_count
  string meta_count_str = file_reader_->read_pos(meta_start, sizeof(uint32_t));
  // 清空`block_metas_`，预分配数组空间，避免 push_back 扩容开销
  if (meta_count_str.size() != sizeof(uint32_t)) {
    LOG_ERROR("read sst meta count failed, file=%s", file_name_.c_str());
    return;
  }
  uint32_t meta_count = get_numeric<uint32_t>(meta_count_str.data());

  block_metas_.clear();
  block_metas_.reserve(meta_count);

  // 依次读取每个 block meta：前面 4 字节是长度，后面是 BlockMeta 编码
  uint32_t pos = meta_start + sizeof(uint32_t);
  // pos是第一个meta起始位置
  for (uint32_t i = 0; i < meta_count; i++) {
    //[meta_count(4B)] [meta1_size(4B)][meta1二进制]
    string meta_size_str = file_reader_->read_pos(pos, sizeof(uint32_t));
    if (meta_size_str.size() != sizeof(uint32_t)) {
      LOG_ERROR("read sst block meta size failed, file=%s", file_name_.c_str());
      return;
    }
    uint32_t meta_size = get_numeric<uint32_t>(meta_size_str.data());
    //**跳过 meta_size 字段，定位到 BlockMeta 二进制数据的起始位置**。
    pos += sizeof(uint32_t);

    string meta_str = file_reader_->read_pos(pos, meta_size);
    if (meta_str.size() != meta_size) {
      LOG_ERROR("read sst block meta failed, file=%s", file_name_.c_str());
      return;
    }
    pos += meta_size;

    BlockMeta meta;
    if (meta.decode(meta_str) != RC::SUCCESS) {
      LOG_ERROR("decode block meta failed, file=%s, meta_idx=%u", file_name_.c_str(), i);
      return;
    }
    block_metas_.push_back(meta);
  }
}

shared_ptr<ObBlock> ObSSTable::read_block_with_cache(uint32_t block_idx) const
{
  // 无缓存时直接读磁盘
  if (block_cache_ == nullptr) {
    return read_block(block_idx);
  }

  // 用 (sst_id, block_id) 拼成单个 uint64 作为缓存 key
  uint64_t cache_key = (static_cast<uint64_t>(sst_id_) << 32) | block_idx;

  shared_ptr<ObBlock> block;
  if (block_cache_->get(cache_key, block)) {
    return block;
  }
  // 读磁盘
  block = read_block(block_idx);
  if (block != nullptr) {
    // 写缓存
    block_cache_->put(cache_key, block);
  }
  return block;
}

shared_ptr<ObBlock> ObSSTable::read_block(uint32_t block_idx) const
{
  // 越界， 返回
  if (block_idx >= block_metas_.size()) {
    return nullptr;
  }
  // 读内存
  const BlockMeta &meta = block_metas_[block_idx];
  // 随机读sst文件
  string data = file_reader_->read_pos(meta.offset_, meta.size_);
  if (data.size() != meta.size_) {
    LOG_ERROR("read block failed, file=%s, block_idx=%u", file_name_.c_str(), block_idx);
    return nullptr;
  }
  // 新建ObBlock智能指针，把二进制 data 反序列化成 block 内部有序 KV 表。**这部分全部是内存计算，没有 IO。**
  shared_ptr<ObBlock> block = make_shared<ObBlock>(comparator_);
  if (block->decode(data) != RC::SUCCESS) {
    LOG_ERROR("decode block failed, file=%s, block_idx=%u", file_name_.c_str(), block_idx);
    return nullptr;
  }
  return block;
}

void ObSSTable::remove() { filesystem::remove(file_name_); }

ObLsmIterator *ObSSTable::new_iterator() { return new TableIterator(get_shared_ptr()); }

void TableIterator::read_block_with_cache()
{
  block_ = sst_->read_block_with_cache(curr_block_idx_);
  block_iterator_.reset(block_->new_iterator());
}

void TableIterator::seek_to_first()
{
  curr_block_idx_ = 0;
  read_block_with_cache();
  block_iterator_->seek_to_first();
}

void TableIterator::seek_to_last()
{
  curr_block_idx_ = block_cnt_ - 1;
  read_block_with_cache();
  block_iterator_->seek_to_last();
}

void TableIterator::next()
{
  block_iterator_->next();
  if (block_iterator_->valid()) {
  } else if (curr_block_idx_ < block_cnt_ - 1) {
    curr_block_idx_++;
    read_block_with_cache();
    block_iterator_->seek_to_first();
  }
}

void TableIterator::seek(const string_view &lookup_key)
{
  curr_block_idx_ = 0;
  // TODO: use binary search
  for (; curr_block_idx_ < block_cnt_; curr_block_idx_++) {
    const auto &block_meta = sst_->block_meta(curr_block_idx_);
    if (sst_->comparator()->compare(
            extract_user_key(block_meta.last_key_), extract_user_key_from_lookup_key(lookup_key)) >= 0) {
      break;
    }
  }
  if (curr_block_idx_ == block_cnt_) {
    block_iterator_ = nullptr;
    return;
  }
  read_block_with_cache();
  block_iterator_->seek(lookup_key);
};

}  // namespace oceanbase
