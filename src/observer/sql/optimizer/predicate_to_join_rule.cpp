/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/predicate_to_join_rule.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"

using namespace std;

namespace
{
/// 判断两个表 ID 集合是否有交集
static bool intersects(const unordered_set<int> &a, const unordered_set<int> &b)
{
  for (int id : b) {
    if (a.find(id) != a.end()) {
      return true;
    }
  }
  return false;
}
}  // namespace

RC PredicateToJoinRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  RC rc = RC::SUCCESS;
  if (oper->type() != LogicalOperatorType::PREDICATE) {
    return rc;
  }

  if (oper->children().size() != 1) {
    return rc;
  }

  unique_ptr<LogicalOperator> &child_oper = oper->children().front();
  if (child_oper->type() != LogicalOperatorType::JOIN) {
    // PREDICATE -> TABLE_GET 由 PredicatePushdownRewriter 处理
    return rc;
  }

  JoinLogicalOperator *join_oper = static_cast<JoinLogicalOperator *>(child_oper.get());
  if (join_oper->children().size() != 2) {
    return rc;
  }

  vector<unique_ptr<Expression>> &predicate_oper_exprs = oper->expressions();
  if (predicate_oper_exprs.size() != 1) {
    return rc;
  }

  unique_ptr<Expression> &predicate_expr = predicate_oper_exprs.front();
  bool                  pushed_any       = false;

  if (predicate_expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conjunction_expr = static_cast<ConjunctionExpr *>(predicate_expr.get());
    // 或 操作的比较，太复杂，现在不考虑
    if (conjunction_expr->conjunction_type() == ConjunctionExpr::Type::OR) {
      return rc;
    }

    vector<unique_ptr<Expression>> &child_exprs = conjunction_expr->children();
    for (auto iter = child_exprs.begin(); iter != child_exprs.end();) {
      if ((*iter)->type() == ExprType::COMPARISON && push_predicate_down(join_oper, *iter)) {
        pushed_any = true;
        iter       = child_exprs.erase(iter);
      } else {
        ++iter;
      }
    }

    if (child_exprs.empty()) {
      // 所有谓词都已下推，消除 predicate 算子，让 join 直接顶上。
      oper       = std::move(child_oper);
      pushed_any = true;
    }
  } else if (predicate_expr->type() == ExprType::COMPARISON) {
    if (push_predicate_down(join_oper, predicate_expr)) {
      oper       = std::move(child_oper);
      pushed_any = true;
    }
  }

  if (pushed_any) {
    change_made = true;
  }
  return rc;
}

bool PredicateToJoinRewriter::push_predicate_down(LogicalOperator *node, unique_ptr<Expression> &predicate)
{
  if (predicate->type() != ExprType::COMPARISON) {
    return false;
  }

  if (node->type() == LogicalOperatorType::TABLE_GET) {
    auto *table_get_oper = static_cast<TableGetLogicalOperator *>(node);
    table_get_oper->predicates().emplace_back(std::move(predicate));
    return true;
  }

  if (node->type() != LogicalOperatorType::JOIN) {
    return false;
  }

  auto *join_oper = static_cast<JoinLogicalOperator *>(node);
  if (join_oper->children().size() != 2) {
    return false;
  }

  unordered_set<int> left_table_ids;
  unordered_set<int> right_table_ids;
  unordered_set<int> predicate_table_ids;

  collect_table_ids(join_oper->children()[0].get(), left_table_ids);
  collect_table_ids(join_oper->children()[1].get(), right_table_ids);
  collect_table_ids(predicate.get(), predicate_table_ids);

  // 谓词只引用左子树表 → 下推到左；只引用右子树表 → 下推到右
  const bool only_left  = is_subset(left_table_ids, predicate_table_ids);   // pred ⊆ left
  const bool only_right = is_subset(right_table_ids, predicate_table_ids);  // pred ⊆ right

  if (only_left) {
    return push_predicate_down(join_oper->children()[0].get(), predicate);
  }
  if (only_right) {
    return push_predicate_down(join_oper->children()[1].get(), predicate);
  }

  // 同时引用左右两表 → 该 join 的连接条件（含非等值）
  const bool refs_left  = intersects(left_table_ids, predicate_table_ids);
  const bool refs_right = intersects(right_table_ids, predicate_table_ids);
  if (refs_left && refs_right) {
    join_oper->add_join_predicate(std::move(predicate));
    return true;
  }

  // 引用的表既不全在左、也不全在右，无法下推
  return false;
}

void PredicateToJoinRewriter::collect_table_ids(LogicalOperator *oper, unordered_set<int> &table_ids)
{
  if (oper->type() == LogicalOperatorType::TABLE_GET) {
    auto *table_get_oper = static_cast<TableGetLogicalOperator *>(oper);
    table_ids.insert(table_get_oper->table()->table_id());
    return;
  }

  for (auto &child : oper->children()) {
    collect_table_ids(child.get(), table_ids);
  }
}

void PredicateToJoinRewriter::collect_table_ids(Expression *expr, unordered_set<int> &table_ids)
{
  if (expr->type() == ExprType::FIELD) {
    auto        *field_expr = static_cast<FieldExpr *>(expr);
    const Table *table      = field_expr->field().table();
    if (table != nullptr) {
      table_ids.insert(table->table_id());
    }
    return;
  }

  function<RC(unique_ptr<Expression> &)> collector = [this, &table_ids](unique_ptr<Expression> &child) -> RC {
    collect_table_ids(child.get(), table_ids);
    return RC::SUCCESS;
  };

  ExpressionIterator::iterate_child_expr(*expr, collector);
}
