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

#include "sql/operator/physical_operator.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "common/lang/vector.h"
#include "common/lang/map.h"

/**
 * @brief Hash Join 算子
 * @ingroup PhysicalOperator
 * @details Implements a single-node hash join using the volcano model.
 * The build side (left child) is fully consumed in open() and stored in a
 * map keyed by the join column.  The probe side (right child) is
 * then iterated one tuple at a time via next(); matching build-side tuples
 * are joined and returned.
 *
 * Currently only supports a single equality condition.
 */
class HashJoinPhysicalOperator : public PhysicalOperator
{
public:
  HashJoinPhysicalOperator();
  virtual ~HashJoinPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::HASH_JOIN; }
  // 两套算子类型标识
  OpType get_op_type() const override { return OpType::INNERHASHJOIN; }

  double calculate_cost(
      LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm) override
  {
    // cost_hashjoin = left × hash_cost + right × hash_probe + output × CPU
    double left_card  = child_log_props[0]->get_card();
    double right_card = child_log_props[1]->get_card();
    double output     = prop->get_card();
    return left_card * cm->hash_cost() + right_card * cm->hash_probe() + output * cm->cpu_op();
  }

  RC     open(Trx *trx) override;
  RC     next() override;
  RC     close() override;
  Tuple *current_tuple() override;

  /**
   * @brief Set the join condition.
   * @details Currently only EQUAL_TO comparisons between two field
   * expressions are supported.
   */
  // 设置 join 等值条件，移动语义转移表达式所有权。
  void set_join_condition(unique_ptr<Expression> &&condition) { join_condition_ = std::move(condition); }

private:
  /** Build the hash table from the left (build) child. */
  RC build_hash_table();

  /**
   * @brief 从一行元组中取出连接键。
   * @details 连接条件是形如 `t1.id = t2.id` 的等值比较，其左右两个字段分别属于
   * 构建侧或探测侧。这里依次尝试用左右两个表达式求值，命中哪一个就用哪一个作为键。
   */
  bool get_join_key(const Tuple &tuple, string &join_key) const;

  PhysicalOperator *left_  = nullptr;
  PhysicalOperator *right_ = nullptr;
  Trx              *trx_   = nullptr;

  /// The equality join condition (e.g. t1.id = t2.id).
  unique_ptr<Expression> join_condition_;

  /// Join map: join key (string) → list of left-side tuples.
  // value是同一个key对应的所有左表元组列表
  using JoinMap = map<string, vector<unique_ptr<ValueListTuple>>>;
  JoinMap hash_table_;

  /// Current state for next() iteration.
  vector<unique_ptr<ValueListTuple>> *current_matches_   = nullptr;
  size_t                              current_match_idx_ = 0;
  Tuple                              *right_tuple_       = nullptr;
  JoinedTuple                         joined_tuple_;

  /// Whether the right side has been fully consumed.
  bool right_exhausted_ = false;
};
