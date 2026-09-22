#include "sql/operator/orderby_logical_operator.h"

OrderByLogicalOperator::OrderByLogicalOperator(
    vector<unique_ptr<Expression>> &&order_by, const vector<bool> &order_by_desc)
    : order_by_(std::move(order_by)), order_by_desc_(order_by_desc)
{}
