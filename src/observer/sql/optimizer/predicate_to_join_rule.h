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

#include "common/lang/vector.h"
#include "common/lang/unordered_set.h"
#include "sql/optimizer/rewrite_rule.h"

class LogicalOperator;
class Expression;

/**
 * @brief 将一些谓词表达式下推到 join 及其子节点中
 * @ingroup Rewriter
 * @details 处理 PREDICATE -> JOIN 的谓词下推。WHERE 中的等值连接条件已经由
 * logical_plan_generator 抽取到对应 join 的 join_predicates 中，这里继续把剩余的谓词下推：
 *  - 只引用单表的过滤条件（如 t2.id = 1）下推到对应的 TableGet；
 *  - 跨表的连接条件（如 t2.id > t3.id，含非等值）下推到对应 Join 的 join_predicates；
 *  - 全部下推干净后消除 Predicate 算子。
 * 通过比较每个谓词表达式与每个算子子树引用的表 ID 集合来判断可下推性，不考虑 OR。
 */
class PredicateToJoinRewriter : public RewriteRule
{
public:
  PredicateToJoinRewriter()          = default;
  virtual ~PredicateToJoinRewriter() = default;

  RC rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made) override;

private:
  /// 收集某个算子子树覆盖的所有表 ID
  void collect_table_ids(LogicalOperator *oper, unordered_set<int> &table_ids);

  /// 收集某个表达式引用的所有表 ID（遍历表达式树中的 FieldExpr）
  void collect_table_ids(Expression *expr, unordered_set<int> &table_ids);

  /**
   * 把一个比较谓词下推到 join 子树中。
   * @param node 下推目标子树（可能是 TableGet 或更深层的 Join）
   * @param predicate 要下推的谓词，成功下推后会被 move 走
   * @return 是否成功下推（未下推时 predicate 保持不变）
   */
  bool push_predicate_down(LogicalOperator *node, unique_ptr<Expression> &predicate);
};
