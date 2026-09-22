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
// Created by WangYunlai on 2022/6/27.
//

#include "sql/operator/update_physical_operator.h"
#include "common/log/log.h"
#include "sql/expr/tuple.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

UpdatePhysicalOperator::UpdatePhysicalOperator(Table *table, const FieldMeta *field, unique_ptr<Expression> value_expr)
    : table_(table), field_(field), value_expr_(std::move(value_expr))
{}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  unique_ptr<PhysicalOperator> &child = children_[0];

  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  trx_ = trx;

  while (OB_SUCC(rc = child->next())) {
    Tuple *tuple = child->current_tuple();
    if (nullptr == tuple) {
      LOG_WARN("failed to get current record: %s", strrc(rc));
      return rc;
    }

    RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
    Record   &record    = row_tuple->record();
    records_.emplace_back(std::move(record));
  }

  child->close();

  // 先收集记录，再逐条 read-modify-write。
  // 记录的 key 复用旧记录的 key，新值写到同一条 key 下（由 LsmTableEngine::update_record_with_trx 完成）。
  for (Record &old_record : records_) {
    RowTuple eval_tuple;
    eval_tuple.set_record(&old_record);
    eval_tuple.set_schema(table_, table_->table_meta().field_metas());

    Value new_value;
    rc = value_expr_->get_value(eval_tuple, new_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to evaluate update value expression: %s", strrc(rc));
      return rc;
    }

    Record new_record;
    rc = new_record.copy_data(old_record.data(), old_record.len());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to copy record data: %s", strrc(rc));
      return rc;
    }
    new_record.set_key(old_record.key());
    new_record.set_rid(old_record.rid());

    rc = table_->set_value_to_record(new_record.data(), new_value, field_);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to set value to record: %s", strrc(rc));
      return rc;
    }

    rc = trx_->update_record(table_, old_record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to update record: %s", strrc(rc));
      return rc;
    }
  }

  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::next()
{
  return RC::RECORD_EOF;
}

RC UpdatePhysicalOperator::close()
{
  return RC::SUCCESS;
}
