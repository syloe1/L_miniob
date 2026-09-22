#pragma once

#include "sql/operator/logical_operator.h"
#include "sql/expr/expression.h"
#include "common/lang/vector.h"

class OrderByLogicalOperator : public LogicalOperator
{
public:
  OrderByLogicalOperator(vector<unique_ptr<Expression>> &&order_by, const vector<bool> &order_by_desc);
  virtual ~OrderByLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::ORDER_BY; }

  const vector<unique_ptr<Expression>> &order_by() const { return order_by_; }
  const vector<bool>                   &order_by_desc() const { return order_by_desc_; }

private:
  vector<unique_ptr<Expression>> order_by_;
  vector<bool>                   order_by_desc_;
};
