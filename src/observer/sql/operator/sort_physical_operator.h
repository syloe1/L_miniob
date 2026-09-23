#pragma once

#include "sql/operator/physical_operator.h"
#include "sql/expr/expression.h"
#include "common/lang/vector.h"
#include "common/lang/memory.h"

/**
 * @brief Sort Physical Operator
 * @ingroup PhysicalOperator
 * @details Reads all tuples from the child operator, sorts them according
 * to the order_by expressions, and returns them one at a time on next().
 */
class SortPhysicalOperator : public PhysicalOperator
{
public:
  SortPhysicalOperator(vector<unique_ptr<Expression>> &&order_by, const vector<bool> &order_by_desc);
  virtual ~SortPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::SORT; }
  OpType               get_op_type() const override { return OpType::ORDERBY; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;
  RC     tuple_schema(TupleSchema &schema) const override;

private:
  // 一条待排序的行：预计算的排序键 + 物化的结果元组。
  struct SortEntry
  {
    vector<Value>                  keys;  ///< 每个 order_by 表达式在改行上的取值
    unique_ptr<ValueListTuple> tuple;    ///< 排序后要输出的元组
  };

  vector<unique_ptr<Expression>> order_by_;
  vector<bool>                   order_by_desc_;
  vector<SortEntry>              entries_;
  int                            current_index_ = 0;
};
