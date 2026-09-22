/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/table/lsm_table_engine.h"
#include "storage/record/heap_record_scanner.h"
#include "common/log/log.h"
#include "storage/index/bplus_tree_index.h"
#include "storage/common/meta_util.h"
#include "storage/db/db.h"
#include "storage/record/lsm_record_scanner.h"
#include "storage/common/codec.h"
#include "storage/trx/lsm_mvcc_trx.h"

RC LsmTableEngine::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  bytes lsm_key;
  uint64_t rid = inc_id_.fetch_add(1);
  Codec::encode(table_->table_id(), rid, lsm_key);
  rc = lsm_->put(string_view((char *)lsm_key.data(), lsm_key.size()), string_view(record.data(), record.len()));
  record.set_key(string((char *)lsm_key.data(), lsm_key.size()));
  return rc;
}

RC LsmTableEngine::insert_record_with_trx(Record &record, Trx *trx)
{
  if (trx == nullptr) {
    return insert_record(record);
  }

  auto lsm_trx = dynamic_cast<LsmMvccTrx *>(trx);
  if (lsm_trx == nullptr || lsm_trx->get_trx() == nullptr) {
    return insert_record(record);
  }

  bytes    lsm_key;
  uint64_t rid = inc_id_.fetch_add(1);
  Codec::encode(table_->table_id(), rid, lsm_key);

  ObLsmTransaction *trx_inner = lsm_trx->get_trx();
  RC rc = trx_inner->put(
      string_view((char *)lsm_key.data(), lsm_key.size()),
      string_view(record.data(), record.len()));
  if (OB_SUCC(rc)) {
    record.set_key(string((char *)lsm_key.data(), lsm_key.size()));
  }
  return rc;
}

RC LsmTableEngine::delete_record(const Record &record)
{
  return lsm_->remove(record.key());
}

RC LsmTableEngine::delete_record_with_trx(const Record &record, Trx *trx)
{
  if (trx == nullptr) {
    return delete_record(record);
  }

  auto lsm_trx = dynamic_cast<LsmMvccTrx *>(trx);
  if (lsm_trx == nullptr || lsm_trx->get_trx() == nullptr) {
    return delete_record(record);
  }

  ObLsmTransaction *trx_inner = lsm_trx->get_trx();
  return trx_inner->remove(record.key());
}

RC LsmTableEngine::update_record_with_trx(const Record &old_record, const Record &new_record, Trx *trx)
{
  // Validate the transaction before touching any data: a late failure after the
  // delete below would silently lose the old record.
  if (trx == nullptr) {
    return RC::UNIMPLEMENTED;
  }

  auto lsm_trx = dynamic_cast<LsmMvccTrx *>(trx);
  if (lsm_trx == nullptr || lsm_trx->get_trx() == nullptr) {
    return RC::UNIMPLEMENTED;
  }

  // Update = delete old + insert new within the same transaction.
  RC rc = delete_record_with_trx(old_record, trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to delete old record in update");
    return rc;
  }

  // For update, we reuse the old record's key (the new value is written
  // under the same key).
  ObLsmTransaction *trx_inner = lsm_trx->get_trx();
  return trx_inner->put(old_record.key(), string_view(new_record.data(), new_record.len()));
}

RC LsmTableEngine::get_record_scanner(RecordScanner *&scanner, Trx *trx, ReadWriteMode mode)
{
  scanner = new LsmRecordScanner(table_, db_->lsm(), trx);
  RC rc = scanner->open_scan();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC LsmTableEngine::open()
{
  // Recover the auto-increment ID from existing LSM data.
  // Iterate through all records for this table and find the maximum
  // encoded inc_id in the keys.
  bytes prefix;
  Codec::encode_table_prefix(table_->table_id(), prefix);

  auto lsm_iter = unique_ptr<ObLsmIterator>(
      lsm_->new_iterator(ObLsmReadOptions()));
  lsm_iter->seek(string_view((char *)prefix.data(), prefix.size()));

  uint64_t max_inc_id = 0;
  while (lsm_iter->valid()) {
    string_view key = lsm_iter->key();
    // The key format is: table_prefix | table_id | rowkey_prefix | inc_id
    // We need to skip past table_prefix, table_id, and rowkey_prefix
    // to get to the inc_id.
    //
    // For simplicity, we just try to find the max inc_id by scanning
    // and counting.  Since inc_id is auto-incremented, the number of
    // records for this table equals the highest inc_id.
    int64_t tid = 0;
    bytes   key_bytes(key.data(), key.data() + key.size());
    Codec::decode(key_bytes, tid);
    if (tid == table_->table_id()) {
      max_inc_id++;
    } else {
      break;  // No more records for this table.
    }
    lsm_iter->next();
  }

  inc_id_.store(max_inc_id);
  LOG_TRACE("Recovered inc_id_=%lu for table %s", max_inc_id, table_->name());
  return RC::SUCCESS;
}
