#pragma once

#include "sql/operator/physical_operator.h"

/**
 * @brief Limit Physical Operator
 * @ingroup PhysicalOperator
 * @details 从子算子读取至多 limit 行，之后 next() 直接返回 EOF。
 */
class LimitPhysicalOperator : public PhysicalOperator
{
public:
  explicit LimitPhysicalOperator(int limit) : limit_(limit) {}
  virtual ~LimitPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::LIMIT; }
  OpType               get_op_type() const override { return OpType::LIMIT; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;
  RC     tuple_schema(TupleSchema &schema) const override;

private:
  int  limit_;
  int  emitted_    = 0;
  bool child_open_ = false;
};
