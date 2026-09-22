/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/include/ob_lsm_transaction.h"
#include "oblsm/util/ob_comparator.h"
#include "common/lang/memory.h"
#include "common/log/log.h"

namespace oceanbase {

// ===========================================================================
// TrxInnerMapIterator
// ===========================================================================

class TrxInnerMapIterator : public ObLsmIterator
{
public:
  explicit TrxInnerMapIterator(const map<string, string> *inner_store)
      : inner_store_(inner_store)
  {}

  bool valid() const override { return iter_ != inner_store_->end(); }

  void seek_to_first() override
  {
    iter_ = inner_store_->begin();
  }

  void seek_to_last() override
  {
    if (inner_store_->empty()) {
      iter_ = inner_store_->end();
    } else {
      iter_ = prev(inner_store_->end());
    }
  }

  void seek(const string_view &key) override
  {
    string k(key.data(), key.size());
    iter_ = inner_store_->lower_bound(k);
  }

  void next() override
  {
    if (valid()) {
      ++iter_;
    }
  }

  string_view key() const override
  {
    return string_view(iter_->first.data(), iter_->first.size());
  }
  string_view value() const override
  {
    return string_view(iter_->second.data(), iter_->second.size());
  }

private:
  const map<string, string>             *inner_store_;
  map<string, string>::const_iterator iter_;
};

// ===========================================================================
// TrxIterator — merges transaction inner store with DB iterator.
// ===========================================================================

class TrxIterator : public ObLsmIterator
{
public:
  TrxIterator(ObLsmIterator *inner_iter, ObLsmIterator *db_iter)
      : inner_iter_(inner_iter), db_iter_(db_iter)
  {}

  ~TrxIterator() override = default;

  bool valid() const override { return current_side_ != NONE; }

  void seek_to_first() override
  {
    inner_iter_->seek_to_first();
    db_iter_->seek_to_first();
    advance();
  }

  void seek_to_last() override
  {
    inner_iter_->seek_to_last();
    db_iter_->seek_to_last();
    // For seek_to_last, we don't implement full reverse merge;
    // just seek to first and advance (functional but not efficient).
    // seek_to_first();
  }

  void seek(const string_view &key) override
  {
    inner_iter_->seek(key);
    db_iter_->seek(key);
    advance();
  }

  void next() override
  {
    if (current_side_ == INNER) {
      inner_iter_->next();
    } else if (current_side_ == DB) {
      db_iter_->next();
    }
    advance();
  }

  string_view key() const override
  {
    if (current_side_ == INNER) return inner_iter_->key();
    if (current_side_ == DB) return db_iter_->key();
    return "";
  }

  string_view value() const override
  {
    if (current_side_ == INNER) return inner_iter_->value();
    if (current_side_ == DB) return db_iter_->value();
    return "";
  }

private:
  enum Side { NONE, INNER, DB };

  void advance()
  {
    for (;;) {
      bool inner_valid = inner_iter_->valid();
      bool db_valid    = db_iter_->valid();

      if (!inner_valid && !db_valid) {
        current_side_ = NONE;
        return;
      }
      if (!inner_valid) {
        current_side_ = DB;
        return;
      }
      if (!db_valid) {
        current_side_ = INNER;
        return;
      }

      // 两侧都有元素：按 key 归并，内层 store 覆盖 DB。
      string_view ik  = inner_iter_->key();
      string_view dk  = db_iter_->key();
      int         cmp = ik.compare(dk);
      if (cmp < 0) {
        // 仅内层有的 key：新写入则输出；tombstone 则静默跳过。
        if (inner_iter_->value().empty()) {
          inner_iter_->next();
          continue;
        }
        current_side_ = INNER;
        return;
      }
      if (cmp > 0) {
        current_side_ = DB;
        return;
      }

      // 同 key：内层 store 覆盖 DB。
      if (inner_iter_->value().empty()) {
        // tombstone：屏蔽两侧同 key 的副本（事务内 delete 生效）。
        inner_iter_->next();
        db_iter_->next();
        continue;
      }
      current_side_ = INNER;
      db_iter_->next();  // 跳过 DB 侧同 key 的旧副本
      return;
    }
  }

  unique_ptr<ObLsmIterator> inner_iter_;
  unique_ptr<ObLsmIterator> db_iter_;
  Side                       current_side_ = NONE;
};

// ===========================================================================
// ObLsmTransaction
// ===========================================================================

ObLsmTransaction::ObLsmTransaction(ObLsm *db, uint64_t ts) : db_(db), ts_(ts) {}

RC ObLsmTransaction::get(const string_view &key, string *value)
{
  if (nullptr == value) {
    return RC::INVALID_ARGUMENT;
  }

  // First check the transaction's inner store.
  string k(key.data(), key.size());
  auto   it = inner_store_.find(k);
  if (it != inner_store_.end()) {
    if (it->second.empty()) {
      // Tombstone: key was deleted in this transaction.
      return RC::NOT_EXIST;
    }
    value->assign(it->second);
    return RC::SUCCESS;
  }

  // Fall back to the database, reading at the transaction's snapshot.
  ObLsmReadOptions options;
  options.seq = static_cast<int64_t>(ts_);
  auto iter = unique_ptr<ObLsmIterator>(db_->new_iterator(options));
  iter->seek(key);
  if (iter->valid() && iter->key() == key) {
    if (iter->value().empty()) {
      return RC::NOT_EXIST;
    }
    value->assign(iter->value().data(), iter->value().size());
    return RC::SUCCESS;
  }
  return RC::NOT_EXIST;
}

RC ObLsmTransaction::put(const string_view &key, const string_view &value)
{
  string k(key.data(), key.size());
  string v(value.data(), value.size());
  inner_store_[k] = v;
  return RC::SUCCESS;
}

RC ObLsmTransaction::remove(const string_view &key)
{
  string k(key.data(), key.size());
  inner_store_[k] = "";  // empty value = tombstone
  return RC::SUCCESS;
}

ObLsmIterator *ObLsmTransaction::new_iterator(ObLsmReadOptions options)
{
  // 事务内迭代器总是读取事务 begin 时拿到的快照（read-your-writes + 快照隔离）。
  options.seq = static_cast<int64_t>(ts_);

  auto inner_iter = make_unique<TrxInnerMapIterator>(&inner_store_);
  inner_iter->seek_to_first();

  auto db_iter = unique_ptr<ObLsmIterator>(db_->new_iterator(options));
  db_iter->seek_to_first();

  return new TrxIterator(inner_iter.release(), db_iter.release());
}

RC ObLsmTransaction::commit()
{
  if (inner_store_.empty()) {
    return RC::SUCCESS;
  }

  // Build a vector of key-value pairs for batch write.
  vector<pair<string, string>> kvs;
  kvs.reserve(inner_store_.size());
  for (auto &entry : inner_store_) {
    kvs.emplace_back(entry.first, entry.second);
  }

  RC rc = db_->batch_put(kvs);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to commit transaction: batch_put failed. rc=%s", strrc(rc));
    return rc;
  }

  inner_store_.clear();
  return RC::SUCCESS;
}

RC ObLsmTransaction::rollback()
{
  inner_store_.clear();
  return RC::SUCCESS;
}

}  // namespace oceanbase
