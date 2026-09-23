#pragma once

#include "sql/operator/logical_operator.h"

/**
 * @brief Limit Logical Operator
 * @ingroup LogicalOperator
 * @details 限制子算子返回的行数。limit < 0 表示不限制。
 */
class LimitLogicalOperator : public LogicalOperator
{
public:
  explicit LimitLogicalOperator(int limit) : limit_(limit) {}
  virtual ~LimitLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::LIMIT; }
  OpType              get_op_type() const override { return OpType::LOGICALLIMIT; }

  int limit() const { return limit_; }

private:
  int limit_;
};
