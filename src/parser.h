#pragma once

#include <string>
#include <vector>

#include "expr.h"

namespace vcqe {

struct SelectItem {
  ExprPtr expr;
  std::string alias;
  bool star = false;
};

struct TableRef {
  std::string name;
  std::string alias;
};

struct OrderTerm {
  ExprPtr expr;
  bool descending = false;
};

struct SelectStatement {
  std::vector<SelectItem> items;
  std::vector<TableRef> tables;
  ExprPtr where;
  std::vector<ExprPtr> group_by;
  ExprPtr having;
  std::vector<OrderTerm> order_by;
  int64_t limit = -1;
};

SelectStatement ParseSelect(const std::string& sql);

}
