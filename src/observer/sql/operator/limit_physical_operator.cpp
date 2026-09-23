#include "sql/operator/limit_physical_operator.h"
#include "common/log/log.h"

RC LimitPhysicalOperator::open(Trx *trx)
{
  emitted_    = 0;
  child_open_ = false;
  if (children_.empty()) {
    return RC::SUCCESS;
  }
  if (limit_ <= 0) {
    // LIMIT 0：一行都不需要，子算子（可能是很贵的排序）就不必打开了。
    return RC::SUCCESS;
  }

  RC rc = children_[0]->open(trx);
  if (OB_SUCC(rc)) {
    child_open_ = true;
  }
  return rc;
}

RC LimitPhysicalOperator::next()
{
  if (!child_open_ || emitted_ >= limit_) {
    return RC::RECORD_EOF;
  }

  RC rc = children_[0]->next();
  if (OB_FAIL(rc)) {
    return rc;
  }

  emitted_++;
  return RC::SUCCESS;
}

RC LimitPhysicalOperator::close()
{
  if (child_open_) {
    children_[0]->close();
    child_open_ = false;
  }
  return RC::SUCCESS;
}

Tuple *LimitPhysicalOperator::current_tuple()
{
  if (children_.empty()) {
    return nullptr;
  }
  return children_[0]->current_tuple();
}

RC LimitPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  if (children_.empty()) {
    return RC::UNIMPLEMENTED;
  }
  // LIMIT 只截断行数，不改变元组的形状，因此直接沿用子算子的 schema。
  // 与 SORT 一样，这个函数会在 open() 之前被 MySQL 协议用来确定列数，
  // 不能依赖 open() 里才产生的东西。
  return children_[0]->tuple_schema(schema);
}
