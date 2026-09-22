/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/hash_join_physical_operator.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "common/log/log.h"

using namespace std;

HashJoinPhysicalOperator::HashJoinPhysicalOperator() {}

RC HashJoinPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 2) {
    LOG_WARN("hash join operator should have 2 children");
    return RC::INTERNAL;
  }

  trx_   = trx;
  left_  = children_[0].get();
  right_ = children_[1].get();

  RC rc = build_hash_table();
  if (rc != RC::SUCCESS) {
    return rc;
  }

  rc = right_->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open right child of hash join");
    return rc;
  }

  right_exhausted_   = false;
  current_matches_   = nullptr;
  current_match_idx_ = 0;
  right_tuple_       = nullptr;

  return RC::SUCCESS;
}

bool HashJoinPhysicalOperator::get_join_key(const Tuple &tuple, string &join_key) const
{
  if (join_condition_ == nullptr || join_condition_->type() != ExprType::COMPARISON) {
    return false;
  }

  auto *cmp_expr = static_cast<ComparisonExpr *>(join_condition_.get());
  if (cmp_expr->comp() != EQUAL_TO) {
    return false;
  }

  // 连接条件 `left = right` 中，构建侧/探测侧的字段可能出现在左边或右边，
  // 这里依次尝试两个表达式，命中哪一个就用哪一个作为连接键。
  for (auto *expr : {cmp_expr->left().get(), cmp_expr->right().get()}) {
    if (expr->type() != ExprType::FIELD) {
      continue;
    }
    Value val;
    if (OB_SUCC(expr->get_value(tuple, val))) {
      join_key = val.to_string();
      return true;
    }
  }
  return false;
}

RC HashJoinPhysicalOperator::build_hash_table()
{
  RC rc = left_->open(trx_);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open left child of hash join");
    return rc;
  }

  while (OB_SUCC(rc = left_->next())) {
    Tuple *left_tuple = left_->current_tuple();
    if (left_tuple == nullptr) {
      break;
    }

    // Determine the join key from the left tuple.
    string join_key;
    get_join_key(*left_tuple, join_key);

    auto value_list = make_unique<ValueListTuple>();
    rc = ValueListTuple::make(*left_tuple, *value_list);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to copy tuple for hash join");
      left_->close();
      return rc;
    }

    hash_table_[join_key].emplace_back(std::move(value_list));
  }

  left_->close();

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to read all tuples from left child. rc=%s", strrc(rc));
    return rc;
  }

  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::next()
{
  while (true) {
    // If we have current matches, return the next one.
    if (current_matches_ != nullptr && current_match_idx_ < current_matches_->size()) {
      auto &left_tuple = (*current_matches_)[current_match_idx_];
      joined_tuple_.set_left(left_tuple.get());
      joined_tuple_.set_right(right_tuple_);
      current_match_idx_++;
      return RC::SUCCESS;
    }

    // Need a new right tuple.
    RC rc = right_->next();
    if (rc == RC::RECORD_EOF) {
      right_exhausted_ = true;
      return RC::RECORD_EOF;
    }
    if (rc != RC::SUCCESS) {
      return rc;
    }

    right_tuple_ = right_->current_tuple();
    if (right_tuple_ == nullptr) {
      continue;
    }

    // Get the join key from the right tuple.
    string join_key;
    get_join_key(*right_tuple_, join_key);

    auto it = hash_table_.find(join_key);
    if (it != hash_table_.end()) {
      current_matches_   = &it->second;
      current_match_idx_ = 0;
      continue;
    }

    current_matches_   = nullptr;
    current_match_idx_ = 0;
  }
}

RC HashJoinPhysicalOperator::close()
{
  hash_table_.clear();
  current_matches_   = nullptr;
  current_match_idx_ = 0;
  if (right_ != nullptr) {
    right_->close();
  }
  return RC::SUCCESS;
}

Tuple *HashJoinPhysicalOperator::current_tuple() { return &joined_tuple_; }
