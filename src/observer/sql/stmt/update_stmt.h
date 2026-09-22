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
 * @brief 更新语句
 * @ingroup Statement
 */
class UpdateStmt : public Stmt
{
public:
  UpdateStmt(Table *table, const FieldMeta *field, unique_ptr<Expression> value_expr, FilterStmt *filter_stmt);
  ~UpdateStmt() override;

  Table               *table() const { return table_; }
  const FieldMeta     *field() const { return field_; }
  unique_ptr<Expression> &value_expr() { return value_expr_; }
  FilterStmt          *filter_stmt() const { return filter_stmt_; }

  StmtType type() const override { return StmtType::UPDATE; }

public:
  static RC create(Db *db, UpdateSqlNode &update_sql, Stmt *&stmt);

private:
  Table                 *table_       = nullptr;
  const FieldMeta       *field_       = nullptr;
  unique_ptr<Expression> value_expr_;
  FilterStmt            *filter_stmt_ = nullptr;
};
