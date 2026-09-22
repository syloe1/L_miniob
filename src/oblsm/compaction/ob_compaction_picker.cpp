/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/compaction/ob_compaction_picker.h"
#include "common/log/log.h"

namespace oceanbase {

// TODO: put it in options
unique_ptr<ObCompaction> TiredCompactionPicker::pick(SSTablesPtr sstables)
{
  // 1. 判断触发条件
  if (sstables->size() < options_->default_run_num) {
    return nullptr;
  }
  // 2. 创建合并任务，基准层级固定为 0
  unique_ptr<ObCompaction> compaction(new ObCompaction(0));
  // TODO(opt): a tricky compaction picker, just pick all sstables if enough sstables.
  // 3. 把所有 SSTable 全部加入待合并列表
  for (size_t i = 0; i < sstables->size(); ++i) {
    size_t tire_i_size = (*sstables)[i].size();
    for (size_t j = 0; j < tire_i_size; ++j) {
      compaction->inputs_[0].emplace_back((*sstables)[i][j]);
    }
  }
  // TODO: LOG_DEBUG for debug
  return compaction;
}

unique_ptr<ObCompaction> LeveledCompactionPicker::pick(SSTablesPtr sstables)
{
  if (!sstables || sstables->empty())
    return nullptr;

  int            num_levels = sstables->size();
  vector<double> scores(num_levels, 0.0);

  // 计算每层分数
  for (int lv = 0; lv < num_levels; ++lv) {
    const auto &level_files = (*sstables)[lv];
    if (lv == 0) {
      scores[lv] = (double)level_files.size() / options_->default_l0_file_num;
    } else {
      // 计算目标大小
      uint64_t target_size = options_->default_l1_level_size;
      for (int i = 1; i < lv; ++i) {
        target_size = target_size * options_->default_level_ratio;
      }
      uint64_t total_size = 0;
      for (auto &sst : level_files)
        total_size += sst->size();
      scores[lv] = (double)total_size / target_size;
    }
  }

  // 选择分数最大的级别
  int    picked_level = -1;
  double max_score    = 1.0;  // 只有 >1 才触发
  for (int lv = 0; lv < num_levels; ++lv) {
    if (scores[lv] > max_score) {
      max_score    = scores[lv];
      picked_level = lv;
    }
  }
  if (picked_level == -1)
    return nullptr;

  unique_ptr<ObCompaction> compaction(new ObCompaction(picked_level));

  if (picked_level == 0) {
    // L0 全部参与
    for (auto &sst : (*sstables)[0]) {
      compaction->inputs_[0].emplace_back(sst);
    }
    // 找出 L1 中与 L0 重叠的文件
    if (num_levels > 1) {
      // 获取 L0 整体的 key 范围（最小起始 key 和最大结束 key）
      string smallest, largest;
      for (auto &sst : compaction->inputs_[0]) {
        if (smallest.empty() || sst->first_key() < smallest)
          smallest = sst->first_key();
        if (largest.empty() || sst->last_key() > largest)
          largest = sst->last_key();
      }
      // 遍历 L1，若与 [smallest, largest] 有交集则加入
      for (auto &sst : (*sstables)[1]) {
        if (sst->last_key() >= smallest && sst->first_key() <= largest) {
          compaction->inputs_[1].emplace_back(sst);
        }
      }
    }
  } else {
    // 非 L0：从该层挑选一个文件，这里简单选第一个
    // 更合理：选文件最大的或重叠最多的
    if ((*sstables)[picked_level].empty())
      return nullptr;
    shared_ptr<ObSSTable> picked_file = (*sstables)[picked_level][0];
    compaction->inputs_[0].emplace_back(picked_file);

    // 从下一层找重叠文件
    int next_level = picked_level + 1;
    if (next_level < num_levels && !(*sstables)[next_level].empty()) {
      string start = picked_file->first_key();
      string end   = picked_file->last_key();
      for (auto &sst : (*sstables)[next_level]) {
        if (sst->last_key() >= start && sst->first_key() <= end) {
          compaction->inputs_[1].emplace_back(sst);
        }
      }
    }
  }

  return compaction;
}

ObCompactionPicker *ObCompactionPicker::create(CompactionType type, ObLsmOptions *options)
{

  switch (type) {
    case CompactionType::TIRED: return new TiredCompactionPicker(options);
    case CompactionType::LEVELED: return new LeveledCompactionPicker(options);
    default: return nullptr;
  }
}

}  // namespace oceanbase
