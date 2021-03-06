//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// convert_query_to_op.h
//
// Identification: src/backend/optimizer/convert_query_to_op.h
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include "backend/optimizer/op_expression.h"
#include "backend/optimizer/column_manager.h"
#include "backend/optimizer/query_operators.h"

#include <memory>

namespace peloton {
namespace optimizer {

std::shared_ptr<OpExpression> ConvertQueryToOpExpression(
  ColumnManager &manager,
  std::shared_ptr<Select> op);

} /* namespace optimizer */
} /* namespace peloton */
