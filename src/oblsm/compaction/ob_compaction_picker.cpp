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
#include "oblsm/util/ob_coding.h"

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

  // 层内 key 的比较必须走 ObInternalKeyComparator，不能用 std::string 的 </<=/>= 。
  // internal key 是 `user_key || seq(8B)`，裸字节比较会把 seq 也一起比进去。例如
  // key1(seq=1000000) 与 key10(seq=1000000)：comparator 判 key1 < key10（user key 前缀），
  // 裸字符串却判 key1 > key10（第 5 字节 seq 低字节 0x40 > '0'）。判错重叠就会漏掉本该参与
  // 合并的下一层文件，那些文件留在原层，与新写出的文件区间重叠 —— 同层 key 重叠不变量被破坏。
  // do_compaction 的归并迭代器和测试里的 check_compaction 用的都是这个 comparator。
  ObInternalKeyComparator cmp;
  // 退化保护：没有 block 的 sstable，其 first_key()/last_key() 是空串，
  // 而 ObInternalKeyComparator 会对不足 SEQ_SIZE 的 key 做 size() - SEQ_SIZE 下溢。
  auto safe_cmp = [&cmp](const string &a, const string &b) -> int {
    if (a.size() < SEQ_SIZE || b.size() < SEQ_SIZE) {
      return a.compare(b);
    }
    return cmp.compare(a, b);
  };
  // 闭区间 [first, last] 与 [start, end] 是否相交
  auto ranges_overlap = [&safe_cmp](const string &first, const string &last, const string &start, const string &end) {
    return safe_cmp(last, start) >= 0 && safe_cmp(first, end) <= 0;
  };

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
        if (smallest.empty() || safe_cmp(sst->first_key(), smallest) < 0)
          smallest = sst->first_key();
        if (largest.empty() || safe_cmp(largest, sst->last_key()) < 0)
          largest = sst->last_key();
      }
      // 遍历 L1，若与 [smallest, largest] 有交集则加入
      if (!smallest.empty() || !largest.empty()) {
        for (auto &sst : (*sstables)[1]) {
          if (ranges_overlap(sst->first_key(), sst->last_key(), smallest, largest)) {
            compaction->inputs_[1].emplace_back(sst);
          }
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
        if (ranges_overlap(sst->first_key(), sst->last_key(), start, end)) {
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
