/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/ob_lsm_impl.h"

#include "common/log/log.h"
#include "common/sys/rc.h"
#include "oblsm/include/ob_lsm.h"
#include "oblsm/ob_manifest.h"
#include "oblsm/ob_lsm_define.h"
#include "oblsm/table/wal/ob_lsm_wal.h"
#include "oblsm/table/ob_merger.h"
#include "oblsm/table/ob_sstable.h"
#include "oblsm/table/ob_sstable_builder.h"
#include "oblsm/util/ob_coding.h"
#include "oblsm/compaction/ob_compaction_picker.h"
#include "oblsm/ob_user_iterator.h"
#include "oblsm/compaction/ob_compaction.h"
#include "oblsm/ob_lsm_define.h"

namespace oceanbase {
ObLsmImpl::ObLsmImpl(const ObLsmOptions &options, const string &path)
    : options_(options), path_(path), mu_(), mem_table_(nullptr), imem_tables_(), manifest_(path)
{
  mem_table_ = make_shared<ObMemTable>();
  sstables_  = make_shared<vector<vector<shared_ptr<ObSSTable>>>>();
  if (options_.type == CompactionType::LEVELED) {
    sstables_->resize(options_.default_levels);
  }

  executor_.init("ObLsmBackground", 1, 1, 60 * 1000);
  block_cache_ =
      std::unique_ptr<ObLRUCache<uint64_t, shared_ptr<ObBlock>>>{new ObLRUCache<uint64_t, shared_ptr<ObBlock>>(1024)};
}

RC ObLsmImpl::recover()
{
  // recover from snapshot -> recover from compaction records -> recover from WAL -> write current snapshot into a new
  // manifest file.
  RC rc = manifest_.open();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open manifest file, rc=%s", strrc(rc));
    return rc;
  }

  std::vector<ObManifestCompaction>      compaction_records;
  std::unique_ptr<ObManifestSnapshot>    snapshot_record;
  std::unique_ptr<ObManifestNewMemtable> new_memtable_record;

  rc = manifest_.recover(snapshot_record, new_memtable_record, compaction_records);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to recover snapshot and manifest records from manifest file, rc=%s", strrc(rc));
    return rc;
  }

  // Recover Oblsm's state from snapshot.
  if (snapshot_record) {
    rc = load_manifest_snapshot(*snapshot_record);
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to load manifest snapshot, rc=%s", strrc(rc));
      return rc;
    }
  }

  // Recover ObLsm's state from compaction records.
  rc = recover_from_manifest_records(compaction_records);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to recover from manifest compaction records, rc=%s", strrc(rc));
    return rc;
  }

  // Recover memtable from the WAL file.
  rc = recover_from_wal(new_memtable_record);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to recover from WAL, rc=%s", strrc(rc));
    return rc;
  }

  // The latest sequence number is now fully restored (from snapshot/compaction
  // records and the WAL). Reflect it so future compaction records carry the
  // correct seq_id.
  manifest_.latest_seq = seq_.load();

  // After recover from the old manifest file, write the snapshot into a new manifest file.
  if (!compaction_records.empty()) {
    rc = write_manifest_snapshot();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to write snapshot into manifest file, rc=%s", strrc(rc));
      return rc;
    }
  }

  return RC::SUCCESS;
}

RC ObLsmImpl::recover_from_wal(const std::unique_ptr<ObManifestNewMemtable> &new_memtable_record)
{
  wal_ = std::make_unique<WAL>();

  if (new_memtable_record) {
    string wal_path = get_wal_path(new_memtable_record->memtable_id);
    vector<WalRecord> wal_records;
    // The WAL file may not exist yet (e.g. a fresh directory, where the manifest
    // already holds an initial NewMemtable record but no WAL has been written).
    // In that case there is nothing to replay.
    if (filesystem::exists(wal_path)) {
      RC rc = wal_->recover(wal_path, wal_records);
      if (rc != RC::SUCCESS) {
        LOG_ERROR("Failed to recover from WAL file %s, rc=%s", wal_path.c_str(), strrc(rc));
        return rc;
      }

      // Replay WAL records into the memtable, and advance seq_ past the highest
      // recovered sequence. Otherwise the version filter in new_user_iterator
      // (which keeps only entries with seq <= seq_) would drop most recovered
      // entries, and future puts would reuse stale sequence numbers.
      uint64_t max_seq = 0;
      bool     has_records = false;
      for (auto &rec : wal_records) {
        mem_table_->put(rec.seq, rec.key, rec.val);
        if (rec.seq > max_seq) {
          max_seq = rec.seq;
        }
        has_records = true;
      }
      if (has_records && max_seq + 1 > seq_.load()) {
        seq_.store(max_seq + 1);
      }

      // Re-open the WAL for new writes using the next memtable id, so we don't
      // append to the WAL we just replayed.
      uint64_t new_memtable_id = new_memtable_record->memtable_id + 1;
      memtable_id_.store(new_memtable_id);
      return wal_->open(get_wal_path(new_memtable_id));
    } else {
      // Fresh start: the manifest records the memtable id but no WAL has been
      // written yet. Reuse that id directly so the first WAL file matches what
      // the manifest points to (otherwise recovery would look at the wrong file).
      memtable_id_.store(new_memtable_record->memtable_id);
      return wal_->open(get_wal_path(new_memtable_record->memtable_id));
    }
  }

  // No previous WAL — start fresh.
  uint64_t new_memtable_id = memtable_id_.fetch_add(1) + 1;
  return wal_->open(get_wal_path(new_memtable_id));
}

RC ObLsm::open(const ObLsmOptions &options, const string &path, ObLsm **dbptr)
{
  RC         rc  = RC::SUCCESS;
  ObLsmImpl *lsm = new ObLsmImpl(options, path);
  *dbptr         = lsm;
  rc             = lsm->recover();
  return rc;
}

RC ObLsmImpl::put(const string_view &key, const string_view &value)
{
  // TODO: if put rate is too high, slow down writes is needed.
  // currently, the writes is stopped when the memtable is full.
  LOG_TRACE("begin to put key=%s, value=%s", key.data(), value.data());
  RC rc = RC::SUCCESS;
  // TODO: currenttly the memtable use skiplist as the underlying data structure,
  // and the skiplist concurently write is not thread safe, so we use mutex here,
  // if the skiplist support `insert_concurrently()` interface, can we remove the mutex?
  unique_lock<mutex> lock(mu_);
  uint64_t           seq = seq_.fetch_add(1);
  // Write WAL
  rc = wal_->put(seq, key, value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (options_.force_sync_new_log) {
    rc = wal_->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to sync wal logs, rc=%s", strrc(rc));
      return rc;
    }
  }
  // write memtable
  mem_table_->put(seq, key, value);
  size_t mem_size = mem_table_->appro_memory_usage();
  if (mem_size > options_.memtable_size) {
    // Thinking point: here vector is used to store imems,
    // but only one imem is stored at most. Is it possible
    // to store more than one imem and what are the implications
    // of storing more than one imem.
    if (imem_tables_.size() >= 1) {
      cv_.wait(lock);
    }
    // check again after get lock(maybe freeze memtable by another thread)
    if (mem_table_->appro_memory_usage() > options_.memtable_size) {
      manifest_.latest_seq = seq;
      try_freeze_memtable();
    } else {
      // if there are multi put threads waiting here, need to notify one thread to
      // continue to write to memtable.
      cv_.notify_one();
    }
  }
  return rc;
}

RC ObLsmImpl::batch_put(const vector<pair<string, string>> &kvs)
{
  if (kvs.empty()) {
    return RC::SUCCESS;
  }

  unique_lock<mutex> lock(mu_);

  // Phase 1: write all entries to WAL first. If any write fails, return
  // without touching the memtable, so the batch is atomic (all-or-nothing).
  struct SeqKV
  {
    uint64_t seq;
    string   key;
    string   value;
  };
  vector<SeqKV> seq_kvs;
  seq_kvs.reserve(kvs.size());
  for (auto &kv : kvs) {
    uint64_t seq = seq_.fetch_add(1);
    RC       rc  = wal_->put(seq, kv.first, kv.second);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to write WAL during batch_put. rc=%s", strrc(rc));
      return rc;
    }
    seq_kvs.push_back({seq, kv.first, kv.second});
  }

  if (options_.force_sync_new_log) {
    RC rc = wal_->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to sync wal logs after batch_put, rc=%s", strrc(rc));
      return rc;
    }
  }

  // Phase 2: WAL fully written, now populate the memtable.
  for (auto &skv : seq_kvs) {
    mem_table_->put(skv.seq, skv.key, skv.value);
  }

  size_t mem_size = mem_table_->appro_memory_usage();
  if (mem_size > options_.memtable_size) {
    manifest_.latest_seq = seq_.load();
    try_freeze_memtable();
    cv_.notify_one();
  }

  return RC::SUCCESS;
}

RC ObLsmImpl::remove(const string_view &key)
{
  // Remove is implemented as a put with empty value (tombstone).
  string empty_value;
  return put(key, empty_value);
}

RC ObLsmImpl::try_freeze_memtable()
{
  RC rc = RC::SUCCESS;
  imem_tables_.emplace_back(mem_table_);
  mem_table_ = make_unique<ObMemTable>();
  // frozen previous wal
  if (!options_.force_sync_new_log) {
    rc = wal_->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to sync wal logs, rc=%s", strrc(rc));
      return rc;
    }
  }

  frozen_wals_.emplace_back(std::move(wal_));
  wal_                     = std::make_unique<WAL>();
  uint64_t new_memtable_id = memtable_id_.fetch_add(1) + 1;
  wal_->open(get_wal_path(new_memtable_id));
  std::shared_ptr<ObLsmBgCompactCtx> background_compaction_ctx = make_shared<ObLsmBgCompactCtx>(new_memtable_id);
  auto bg_task = [this, background_compaction_ctx]() { this->background_compaction(background_compaction_ctx); };
  int  ret     = executor_.execute(bg_task);
  if (ret != 0) {
    rc = RC::INTERNAL;
    LOG_WARN("fail to execute background compaction task");
  }
  return rc;
}

void ObLsmImpl::background_compaction(std::shared_ptr<ObLsmBgCompactCtx> ctx)
{
  unique_lock<mutex> lock(mu_);
  if (imem_tables_.size() >= 1) {
    shared_ptr<ObMemTable> imem       = imem_tables_.back();
    shared_ptr<WAL>        frozen_wal = frozen_wals_.back();

    // TODO: build memtable to sst shouldn't in lock?
    build_sstable(imem);
    imem_tables_.pop_back();
    frozen_wals_.pop_back();
    manifest_.push(ObManifestNewMemtable{ctx->new_memtable_id});

    ::remove(frozen_wal->filename().c_str());

    lock.unlock();
    cv_.notify_one();

    // TODO: trig compaction at more scenarios, for example,
    // seek compaction in
    // leveldb(https://github.com/google/leveldb/blob/578eeb702ec0fbb6b9780f3d4147b1076630d633/db/version_set.cc#L650).
    if (!compacting_) {
      compacting_.store(true);
      try_major_compaction();
      compacting_.store(false);
    }
    return;
  }
}

void ObLsmImpl::try_major_compaction()
{
  unique_lock<mutex>             lock(mu_);
  unique_ptr<ObCompactionPicker> picker(ObCompactionPicker::create(options_.type, &options_));
  unique_ptr<ObCompaction>       picked = picker->pick(sstables_);
  ObManifestCompaction           mf_record;
  lock.unlock();
  if (picked == nullptr || picked->size() == 0) {
    return;
  }
  vector<shared_ptr<ObSSTable>> results = do_compaction(picked.get());
  if (results.empty()) {
    // do_compaction 失败时会丢弃部分结果并返回空。此时绝不能替换 sstables_
    // 或删除输入文件：输入的数据既不在新层级里、文件也没了，就是永久丢失。
    LOG_WARN("compaction produced no output; keeping input sstables untouched");
    return;
  }

  SSTablesPtr new_sstables = make_shared<vector<vector<shared_ptr<ObSSTable>>>>();
  lock.lock();
  size_t levels_size        = sstables_->size();
  bool   insert_new_sstable = false;
  auto   find_sstable       = [](const vector<shared_ptr<ObSSTable>> &picked, const shared_ptr<ObSSTable> &sstable) {
    for (auto &p : picked) {
      if (p->sst_id() == sstable->sst_id()) {
        return true;
      }
    }
    return false;
  };

  vector<shared_ptr<ObSSTable>> picked_sstables;
  picked_sstables      = picked->inputs(0);
  const auto &level_i1 = picked->inputs(1);
  if (level_i1.size() > 0) {
    picked_sstables.insert(picked_sstables.end(), level_i1.begin(), level_i1.end());
  }
  // TODO: unify the new sstables logic in all compaction type
  if (options_.type == CompactionType::TIRED) {
    for (int i = levels_size - 1; i >= 0; --i) {
      const vector<shared_ptr<ObSSTable>> &level_i = sstables_->at(i);
      for (auto &sstable : level_i) {
        if (find_sstable(picked_sstables, sstable)) {
          if (!insert_new_sstable) {
            new_sstables->insert(new_sstables->begin(), results);
            insert_new_sstable = true;
          }
        } else {
          new_sstables->insert(new_sstables->begin(), level_i);
          break;
        }
      }
    }
  } else if (options_.type == CompactionType::LEVELED) {
    // Leveled compaction 将 level_ 与 level_+1 的输入合并后，把结果放回 level_+1。
    // 这里重建每一层：去掉参与合并的 sstable，并把结果插入输出层。
    size_t output_level = picked->level() + 1;
    for (size_t i = 0; i < levels_size; ++i) {
      const vector<shared_ptr<ObSSTable>> &level_i = sstables_->at(i);
      vector<shared_ptr<ObSSTable>>        new_level;
      new_level.reserve(level_i.size());
      for (auto &sstable : level_i) {
        if (!find_sstable(picked_sstables, sstable)) {
          new_level.emplace_back(sstable);
        }
      }
      if (i == output_level) {
        new_level.insert(new_level.end(), results.begin(), results.end());
      }
      new_sstables->emplace_back(std::move(new_level));
    }
  }

  sstables_ = new_sstables;
  lock.unlock();

  // remove from disk
  for (auto &sstable : picked_sstables) {
    sstable->remove();
  }

  // Record the compaction's effect on the manifest so recovery can reconstruct
  // the correct SSTable set: the picked inputs are deleted, and the new output
  // tables are added at the output level.
  mf_record.compaction_type = options_.type;
  for (const auto &sstable : picked->inputs(0)) {
    mf_record.deleted_tables.emplace_back(sstable->sst_id(), picked->level());
  }
  for (const auto &sstable : picked->inputs(1)) {
    mf_record.deleted_tables.emplace_back(sstable->sst_id(), picked->level() + 1);
  }
  int result_level = (options_.type == CompactionType::LEVELED) ? picked->level() + 1 : 0;
  for (const auto &sstable : results) {
    mf_record.added_tables.emplace_back(sstable->sst_id(), result_level);
  }

  mf_record.sstable_sequence_id = sstable_id_.load();
  mf_record.seq_id              = manifest_.latest_seq;
  manifest_.push(std::move(mf_record));
  try_major_compaction();
}

vector<shared_ptr<ObSSTable>> ObLsmImpl::do_compaction(ObCompaction *picked)
{
  vector<shared_ptr<ObSSTable>> results;
  if (picked == nullptr) {
    return results;
  }

  // 为每个输入 SSTable（来自 level_ 与 level_+1 两层）创建迭代器。
  vector<unique_ptr<ObLsmIterator>> iters;
  for (int which = 0; which < 2; ++which) {
    for (const auto &sst : picked->inputs(which)) {
      iters.emplace_back(sst->new_iterator());
    }
  }
  if (iters.empty()) {
    return results;
  }

  // 用内部 key 比较器将多个迭代器合并成一个有序迭代器。
  unique_ptr<ObLsmIterator> merged(new_merging_iterator(&internal_key_comparator_, std::move(iters)));
  if (merged == nullptr) {
    return results;
  }

  // 用 ObSSTableBuilder 把合并后的 KV 写入新的 SSTable，超过 table_size 则换一个新文件。
  unique_ptr<ObSSTableBuilder> builder = make_unique<ObSSTableBuilder>(&default_comparator_, block_cache_.get());
  bool     builder_started = false;
  uint64_t sst_id          = 0;

  // 失败时丢弃已经写出的部分结果（连文件一起删掉）并返回空。
  // 约定：返回空表示本次合并失败，调用方必须放弃合并、保留输入 SSTable，
  // 否则输入被删而结果为空会造成数据丢失。
  auto discard_partial_output = [&results]() {
    for (auto &sstable : results) {
      sstable->remove();
    }
    results.clear();
  };

  merged->seek_to_first();
  while (merged->valid()) {
    if (!builder_started) {
      sst_id = sstable_id_.fetch_add(1);
      RC rc  = builder->start_build(sst_id, get_sstable_path(sst_id));
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to start build sstable, rc=%s", strrc(rc));
        discard_partial_output();
        return results;
      }
      builder_started = true;
    }

    RC rc = builder->add(merged->key(), merged->value());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to add kv into sstable, rc=%s", strrc(rc));
      discard_partial_output();
      return results;
    }
    merged->next();

    if (builder->curr_file_size() >= options_.table_size) {
      builder->finish_build_table();
      results.emplace_back(builder->get_built_table());
      builder_started = false;
    }
  }

  if (builder_started) {
    builder->finish_build_table();
    results.emplace_back(builder->get_built_table());
  }

  return results;
}

void ObLsmImpl::build_sstable(shared_ptr<ObMemTable> imem)
{
  unique_ptr<ObSSTableBuilder> tb = make_unique<ObSSTableBuilder>(&default_comparator_, block_cache_.get());

  uint64_t sstable_id = sstable_id_.fetch_add(1);
  tb->build(imem, get_sstable_path(sstable_id), sstable_id);
  // unique_lock<mutex> lock(mu_);

  ObManifestCompaction record;
  record.compaction_type     = options_.type;
  record.sstable_sequence_id = sstable_id_.load();
  record.seq_id              = manifest_.latest_seq;

  // TODO: unify the build sstable logic in all compaction type
  if (options_.type == CompactionType::TIRED) {
    // TODO: record the changes for tired compaction
    // here we use `level_i` to store `run_i`
    sstables_->insert(sstables_->begin(), {tb->get_built_table()});
  } else if (options_.type == CompactionType::LEVELED) {
    sstables_->at(0).emplace_back(tb->get_built_table());
    record.added_tables.emplace_back(sstable_id, 0);
    manifest_.push(std::move(record));
  }
}

string ObLsmImpl::get_sstable_path(uint64_t sstable_id)
{
  return filesystem::path(path_) / (to_string(sstable_id) + SSTABLE_SUFFIX);
}

string ObLsmImpl::get_wal_path(uint64_t memtable_id)
{
  return filesystem::path(path_) / (to_string(memtable_id) + WAL_SUFFIX);
}

RC ObLsmImpl::get(const string_view &key, string *value)
{
  RC rc = RC::SUCCESS;
  // new_iterator() acquires mu_ itself and returns an iterator over a
  // consistent snapshot (shared_ptr to memtable/imem/sstables), so holding
  // mu_ here would self-deadlock on the non-recursive mutex.
  auto iter = unique_ptr<ObLsmIterator>(new_iterator(ObLsmReadOptions{}));
  iter->seek(key);
  if (iter->valid() && iter->key() == key) {
    if (iter->value().empty()) {
      rc = RC::NOT_EXIST;
    } else {
      value->assign(iter->value());
    }
  } else {
    rc = RC::NOT_EXIST;
  }
  return rc;
}

ObLsmIterator *ObLsmImpl::new_iterator(ObLsmReadOptions options)
{
  unique_lock<mutex>     lock(mu_);
  shared_ptr<ObMemTable> mem = mem_table_;

  shared_ptr<ObMemTable> imm = nullptr;
  if (!imem_tables_.empty()) {
    imm = imem_tables_.back();
  }
  vector<shared_ptr<ObSSTable>> sstables;
  for (auto &level : *sstables_) {
    sstables.insert(sstables.end(), level.begin(), level.end());
  }
  lock.unlock();
  vector<unique_ptr<ObLsmIterator>> iters;
  iters.emplace_back(mem->new_iterator());
  if (imm != nullptr) {
    iters.emplace_back(imm->new_iterator());
  }
  for (const auto &sst : sstables) {
    iters.emplace_back(sst->new_iterator());
  }

  return new_user_iterator(
      new_merging_iterator(&internal_key_comparator_, std::move(iters)), options.seq == -1 ? seq_.load() : options.seq);
}

ObLsmTransaction *ObLsmImpl::begin_transaction()
{
  // 取当前 seq 作为快照，并预留一个序列号：保证本事务提交时 batch_put/put
  // 分配的 seq 严格大于快照（快照隔离：提交的写入对旧快照不可见）。
  uint64_t ts = seq_.load();
  seq_.fetch_add(1);
  return new ObLsmTransaction(this, ts);
}

void ObLsmImpl::dump_sstables()
{
  unique_lock<mutex> lock(mu_);
  int                level = sstables_->size();
  for (int i = 0; i < level; i++) {
    cout << "level " << i << endl;
    int level_size = 0;
    for (auto &sst : sstables_->at(i)) {
      cout << sst->sst_id() << ": " << sst->size() << ";";
      level_size += sst->size();
    }
    cout << "level size " << level_size << endl;
  }
}

RC ObLsmImpl::recover_from_manifest_records(const std::vector<ObManifestCompaction> &records)
{
  std::vector<std::vector<uint64_t>> tmp_sstables;
  for (size_t i = 0; i < options_.default_levels; i++) {
    tmp_sstables.emplace_back();
  }
  for (auto &record : records) {
    // assert(sstable_id_ < record.sstable_sequence_id);
    sstable_id_ = record.sstable_sequence_id;
    seq_        = record.seq_id;
    // Added tables
    for (auto &info : record.added_tables) {
      uint32_t level      = info.level;
      uint64_t sstable_id = info.sstable_id;
      ASSERT(level < options_.default_levels, "level shouldn't greater than or equal to default level size");
      tmp_sstables[level].push_back(sstable_id);
    }
    // Deleted tables
    for (auto &info : record.deleted_tables) {
      uint32_t level = info.level;
      uint32_t sid   = info.sstable_id;
      ASSERT(level < options_.default_levels, "level shouldn't greater than or equal to default level size");
      auto del_iter = std::find(tmp_sstables[level].begin(), tmp_sstables[level].end(), sid);
      tmp_sstables[level].erase(del_iter);
    }
  }

  RC rc = load_manifest_sstable(tmp_sstables);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to load sstables, rc=%s", strrc(rc));
    return rc;
  }
  return RC::SUCCESS;
}

RC ObLsmImpl::load_manifest_snapshot(const ObManifestSnapshot &snapshot)
{
  seq_        = snapshot.seq;
  sstable_id_ = snapshot.sstable_id;
  RC rc       = load_manifest_sstable(snapshot.sstables);
  return rc;
}

RC ObLsmImpl::load_manifest_sstable(const std::vector<std::vector<uint64_t>> &sstables)
{
  // After Getting the final state of lsm tree, recovering the system's state from tmp_sstables
  size_t cur_level_idx = 0;
  for (auto &sst_ids : sstables) {
    auto &cur_level = sstables_->at(cur_level_idx++);
    for (auto &sst_id : sst_ids) {
      auto filename = get_sstable_path(sst_id);
      auto sstable  = std::make_shared<ObSSTable>(sst_id, filename, &default_comparator_, block_cache_.get());
      sstable->init();
      cur_level.emplace_back(sstable);
    }
  }
  return RC::SUCCESS;
}

RC ObLsmImpl::write_manifest_snapshot()
{
  ObManifestSnapshot    snapshot;
  ObManifestNewMemtable new_memtable;
  snapshot.seq             = seq_.load();
  snapshot.sstable_id      = sstable_id_.load();
  snapshot.compaction_type = options_.type;
  snapshot.sstables.resize(sstables_->size());
  new_memtable.memtable_id = memtable_id_.load();
  for (size_t i = 0; i < sstables_->size(); ++i) {
    auto &level = sstables_->at(i);
    for (size_t j = 0; j < level.size(); ++j) {
      snapshot.sstables[i].push_back(level[j]->sst_id());
    }
  }
  RC rc = manifest_.redirect(snapshot, new_memtable);
  if (OB_FAIL(rc)) {
    LOG_ERROR("Failed to redirect snapshot");
    return rc;
  }
  return RC::SUCCESS;
}

}  // namespace oceanbase