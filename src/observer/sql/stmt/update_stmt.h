/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/5/22.
//

#pragma once

#include "common/sys/rc.h"
#include "sql/expr/expression.h"
#include "sql/stmt/stmt.h"

class Table;
class FieldMeta;
class FilterStmt;

/**
 * @brief 单个 SET 赋值：目标字段 + 值表达式
 * @ingroup Statement
 */
struct UpdateAssignment
{
  const FieldMeta       *field = nullptr;  ///< 目标字段
  unique_ptr<Expression> value_expr;       ///< 值表达式
};

/**
 * @brief 更新语句
 * @ingroup Statement
 */
class UpdateStmt : public Stmt
{
public:
  UpdateStmt(Table *table, vector<UpdateAssignment> assignments, FilterStmt *filter_stmt);
  ~UpdateStmt() override;

  Table                    *table() const { return table_; }
  vector<UpdateAssignment> &assignments() { return assignments_; }
  FilterStmt               *filter_stmt() const { return filter_stmt_; }

  StmtType type() const override { return StmtType::UPDATE; }

public:
  static RC create(Db *db, UpdateSqlNode &update_sql, Stmt *&stmt);

private:
  Table                    *table_       = nullptr;
  vector<UpdateAssignment>  assignments_;
  FilterStmt               *filter_stmt_ = nullptr;
};
