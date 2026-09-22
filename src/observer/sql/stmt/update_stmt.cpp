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

#include "sql/stmt/update_stmt.h"
#include "common/log/log.h"
#include "sql/parser/expression_binder.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

UpdateStmt::UpdateStmt(Table *table, const FieldMeta *field, unique_ptr<Expression> value_expr, FilterStmt *filter_stmt)
    : table_(table), field_(field), value_expr_(std::move(value_expr)), filter_stmt_(filter_stmt)
{}

UpdateStmt::~UpdateStmt()
{
  if (filter_stmt_ != nullptr) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC UpdateStmt::create(Db *db, UpdateSqlNode &update_sql, Stmt *&stmt)
{
  stmt = nullptr;

  const char *table_name = update_sql.relation_name.c_str();
  if (nullptr == db || nullptr == table_name) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const FieldMeta *field = table->table_meta().field(update_sql.attribute_name.c_str());
  if (nullptr == field) {
    LOG_WARN("no such field. table=%s, field=%s", table_name, update_sql.attribute_name.c_str());
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  // 绑定 SET 右侧表达式（C_BALANCE -> FieldExpr，+ -> ArithmeticExpr）
  BinderContext binder_context;
  binder_context.add_table(table);
  ExpressionBinder expression_binder(binder_context);

  vector<unique_ptr<Expression>> bound_expressions;
  RC                             rc = expression_binder.bind_expression(update_sql.value, bound_expressions);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to bind update value expression. rc=%s", strrc(rc));
    return rc;
  }
  if (bound_expressions.size() != 1) {
    LOG_WARN("update value expression must resolve to a single expression. got %zu", bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  unordered_map<string, Table *> table_map;
  table_map.insert(pair<string, Table *>(string(table_name), table));

  FilterStmt *filter_stmt = nullptr;
  rc = FilterStmt::create(db,
      table,
      &table_map,
      update_sql.conditions.data(),
      static_cast<int>(update_sql.conditions.size()),
      filter_stmt);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create filter statement. rc=%d:%s", rc, strrc(rc));
    return rc;
  }

  stmt = new UpdateStmt(table, field, std::move(bound_expressions[0]), filter_stmt);
  return RC::SUCCESS;
}
