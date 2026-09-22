#include "sql/operator/sort_physical_operator.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "common/log/log.h"
#include <algorithm>

using namespace std;

SortPhysicalOperator::SortPhysicalOperator(
    vector<unique_ptr<Expression>> &&order_by, const vector<bool> &order_by_desc)
    : order_by_(std::move(order_by)), order_by_desc_(order_by_desc)
{}

RC SortPhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  PhysicalOperator *child = children_[0].get();
  RC                rc    = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child of sort operator");
    return rc;
  }

  // Read all tuples from the child into memory, and evaluate the order_by
  // expressions eagerly while the child tuple is still valid. This is important
  // for multi-table queries: the expressions are resolved against the child
  // tuple (which keeps table/field information), instead of relying on column
  // names that may be ambiguous.
  while (OB_SUCC(rc = child->next())) {
    Tuple *child_tuple = child->current_tuple();
    if (child_tuple == nullptr) {
      break;
    }

    SortEntry entry;
    entry.keys.reserve(order_by_.size());
    for (auto &expr : order_by_) {
      Value key;
      expr->get_value(*child_tuple, key);
      entry.keys.emplace_back(std::move(key));
    }

    entry.tuple = make_unique<ValueListTuple>();
    rc          = ValueListTuple::make(*child_tuple, *entry.tuple);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to copy tuple for sorting");
      return rc;
    }
    entries_.emplace_back(std::move(entry));
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to read all tuples from child. rc=%s", strrc(rc));
    return rc;
  }

  child->close();

  // Store the schema from the first tuple (or empty).
  if (!entries_.empty()) {
    const int cell_num = entries_[0].tuple->cell_num();
    for (int i = 0; i < cell_num; i++) {
      TupleCellSpec spec;
      entries_[0].tuple->spec_at(i, spec);
      schema_.append_cell(spec);
    }
  }

  // Sort the collected tuples based on the pre-computed order_by keys.
  if (!order_by_.empty()) {
    std::sort(entries_.begin(), entries_.end(), [this](const SortEntry &a, const SortEntry &b) {
      for (size_t i = 0; i < order_by_.size(); i++) {
        int cmp = a.keys[i].compare(b.keys[i]);
        if (cmp != 0) {
          return order_by_desc_[i] ? (cmp > 0) : (cmp < 0);
        }
      }
      return false;  // equal
    });
  }

  current_index_ = 0;
  return RC::SUCCESS;
}

RC SortPhysicalOperator::next()
{
  if (current_index_ < static_cast<int>(entries_.size())) {
    current_index_++;
    return RC::SUCCESS;
  }
  return RC::RECORD_EOF;
}

RC SortPhysicalOperator::close()
{
  entries_.clear();
  current_index_ = 0;
  return RC::SUCCESS;
}

Tuple *SortPhysicalOperator::current_tuple()
{
  if (current_index_ > 0 && current_index_ <= static_cast<int>(entries_.size())) {
    return entries_[current_index_ - 1].tuple.get();
  }
  return nullptr;
}

RC SortPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  schema = schema_;
  return RC::SUCCESS;
}
