/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
   miniob is licensed under Mulan PSL v2.
   You can use this software according to the terms and conditions of the Mulan PSL v2.
   You may obtain a copy of Mulan PSL v2 at:
            http://license.coscl.org.cn/MulanPSL2
   THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
   EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
   MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
   See the Mulan PSL v2 for more details. */

#include "oblsm/table/wal/ob_lsm_wal.h"
#include "common/log/log.h"
#include "oblsm/util/ob_file_reader.h"
#include "oblsm/util/ob_coding.h"

namespace oceanbase {
RC WAL::open(const std::string &filename)
{
  // 1. 记录当前 WAL 文件路径
  filename_ = filename;

  // 2. 创建文件写入器，以「追加模式」打开文件
  file_writer_ = ObFileWriter::create_file_writer(filename, true /* append */);

  // 3. 判空：创建失败则打日志、返回文件打开错误码
  if (file_writer_ == nullptr) {
    LOG_ERROR("Failed to create file writer for WAL: %s", filename.c_str());
    return RC::IOERR_OPEN;
  }

  // 4. 打开成功，返回成功状态码
  return RC::SUCCESS;
}

RC WAL::put(uint64_t seq, string_view key, string_view val)
{
  if (file_writer_ == nullptr) {
    LOG_ERROR("WAL file writer is null");
    return RC::INTERNAL;
  }
  // 互斥锁
  unique_lock<mutex> lock(mu_);

  // Format: | seq (uint64_t) | key_len (size_t) | key | val_len (size_t) | val |
  string record;
  // 1. 写入 8 字节序列号
  put_numeric<uint64_t>(&record, seq);

  // 2. 写入 key 长度
  put_numeric<size_t>(&record, key.size());
  // 3. 追加 key 原始数据
  record.append(key.data(), key.size());

  // 4. 写入 value 长度
  put_numeric<size_t>(&record, val.size());
  // 5. 追加 value 原始数据
  record.append(val.data(), val.size());
  // 落地到文件
  RC rc = file_writer_->write(record);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to write WAL record");
    return rc;
  }
  return RC::SUCCESS;
}

RC WAL::sync()
{
  // 1. 检查文件写入器是否有效
  // 没有要刷盘的数据
  if (file_writer_ == nullptr) {
    return RC::SUCCESS;
  }
  // 2. 调用底层接口强制刷盘
  return file_writer_->flush();
}

RC WAL::recover(const std::string &wal_file, std::vector<WalRecord> &wal_records)
{
  // Read the entire WAL file.
  unique_ptr<ObFileReader> reader = ObFileReader::create_file_reader(wal_file);
  // 打开失败
  if (reader == nullptr) {
    LOG_WARN("Failed to open WAL file for recovery: %s", wal_file.c_str());
    return RC::IOERR_OPEN;
  }
  // 处理空文件
  uint32_t file_size = reader->file_size();
  if (file_size == 0) {
    return RC::SUCCESS;
  }
  // 一次性读取文件
  string data = reader->read_pos(0, file_size);
  if (data.empty()) {
    LOG_WARN("Failed to read WAL file: %s", wal_file.c_str());
    return RC::IOERR_READ;
  }

  // Parse records sequentially.
  const char *p        = data.data();
  const char *data_end = p + data.size();

  while (p < data_end) {
    // 校验：剩余数据至少能放下 seq + key_len，否则是残缺日志，直接退出
    if (data_end - p < static_cast<int>(sizeof(uint64_t) + sizeof(size_t))) {
      break;  // Partial record at end, skip.
    }

    uint64_t seq = get_numeric<uint64_t>(p);
    p += sizeof(uint64_t);
    size_t key_len = get_numeric<size_t>(p);
    p += sizeof(size_t);

    if (data_end - p < static_cast<int>(key_len + sizeof(size_t))) {
      break;
    }

    string key(p, key_len);
    p += key_len;

    size_t val_len = get_numeric<size_t>(p);
    p += sizeof(size_t);

    if (data_end - p < static_cast<int>(val_len)) {
      break;
    }

    string val(p, val_len);
    p += val_len;
    // 存入容器
    wal_records.emplace_back(seq, std::move(key), std::move(val));
  }

  return RC::SUCCESS;
}

}  // namespace oceanbase
