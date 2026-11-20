#include "expr.h"

#include <cmath>
#include <cstdlib>

#include "error.h"
#include "util.h"

namespace vcqe {
namespace {

int64_t PowerOfTen(uint8_t exponent) {
  int64_t value = 1;
  for (uint8_t i = 0; i < exponent; ++i) {
    value *= 10;
  }
  return value;
}

struct Numeric {
  DataType type;
  uint8_t scale;
};

Numeric CommonNumeric(DataType left, uint8_t left_scale, DataType right, uint8_t right_scale) {
  if (left == DataType::kString || right == DataType::kString) {
    Check(left == right, "cannot mix text and numeric values");
    return {DataType::kString, 0};
  }
  if (left == DataType::kDouble || right == DataType::kDouble) {
    return {DataType::kDouble, 0};
  }
  if (left == DataType::kDecimal || right == DataType::kDecimal) {
    const uint8_t scale = left_scale > right_scale ? left_scale : right_scale;
    return {DataType::kDecimal, scale};
  }
  if (left == DataType::kDate && right == DataType::kDate) {
    return {DataType::kDate, 0};
  }
  return {DataType::kInt64, 0};
}

double ValueAsDouble(const Value& value) {
  switch (value.type) {
  case DataType::kDouble:
    return value.real;
  case DataType::kDecimal:
    return static_cast<double>(value.integer) / static_cast<double>(PowerOfTen(value.scale));
  case DataType::kString:
    return std::stod(value.text);
  default:
    return static_cast<double>(value.integer);
  }
}

Value CoerceValue(const Value& value, DataType target, uint8_t scale) {
  if (value.is_null) {
    return Value::Null(target);
  }
  switch (target) {
  case DataType::kString:
    Check(value.type == DataType::kString, "expected a text value");
    return value;
  case DataType::kDate:
    if (value.type == DataType::kString) {
      return Value::Date(ParseDate(value.text));
    }
    return Value::Date(static_cast<int32_t>(value.integer));
  case DataType::kDouble:
    return Value::Real(ValueAsDouble(value));
  case DataType::kDecimal: {
    if (value.type == DataType::kDecimal) {
      if (value.scale == scale) {
        return Value::Decimal(value.integer, scale);
      }
      if (value.scale < scale) {
        return Value::Decimal(value.integer * PowerOfTen(scale - value.scale), scale);
      }
      const int64_t factor = PowerOfTen(value.scale - scale);
      return Value::Decimal(value.integer / factor, scale);
    }
    if (value.type == DataType::kString) {
      return Value::Decimal(ParseDecimal(value.text, scale), scale);
    }
    if (value.type == DataType::kDouble) {
      return Value::Decimal(static_cast<int64_t>(std::llround(value.real * PowerOfTen(scale))),
                            scale);
    }
    return Value::Decimal(value.integer * PowerOfTen(scale), scale);
  }
  case DataType::kInt32:
  case DataType::kInt64: {
    if (value.type == DataType::kString) {
      return Value::Int(std::stoll(value.text), target);
    }
    if (value.type == DataType::kDouble) {
      return Value::Int(static_cast<int64_t>(value.real), target);
    }
    if (value.type == DataType::kDecimal) {
      return Value::Int(value.integer / PowerOfTen(value.scale), target);
    }
    return Value::Int(value.integer, target);
  }
  }
  return value;
}

void FoldConstants(Expr* expr) {
  if (expr->children[0]->kind != ExprKind::kConstant ||
      expr->children[1]->kind != ExprKind::kConstant) {
    return;
  }
  const Value folded = EvalRowValue(*expr, {});
  expr->kind = ExprKind::kConstant;
  expr->constant = folded;
  expr->children.clear();
}

ExprPtr WrapCast(ExprPtr child, DataType type, uint8_t scale) {
  if (child->type == type && child->scale == scale) {
    return child;
  }
  if (child->kind == ExprKind::kConstant) {
    child->constant = CoerceValue(child->constant, type, scale);
    child->type = type;
    child->scale = scale;
    return child;
  }
  ExprPtr cast = std::make_shared<Expr>();
  cast->kind = ExprKind::kCast;
  cast->type = type;
  cast->scale = scale;
  cast->children.push_back(std::move(child));
  return cast;
}

}

ExprPtr MakeColumn(const std::string& table, const std::string& name) {
  ExprPtr expr = std::make_shared<Expr>();
  expr->kind = ExprKind::kColumn;
  expr->table_name = table;
  expr->column_name = name;
  return expr;
}

ExprPtr MakeConstant(Value value) {
  ExprPtr expr = std::make_shared<Expr>();
  expr->kind = ExprKind::kConstant;
  expr->type = value.type;
  expr->scale = value.scale;
  expr->constant = std::move(value);
  return expr;
}

ExprPtr MakeCompare(CompareOp op, ExprPtr left, ExprPtr right) {
  ExprPtr expr = std::make_shared<Expr>();
  expr->kind = ExprKind::kCompare;
  expr->compare = op;
  expr->children.push_back(std::move(left));
  expr->children.push_back(std::move(right));
  return expr;
}

ExprPtr MakeLogical(LogicalOp op, std::vector<ExprPtr> children) {
  ExprPtr expr = std::make_shared<Expr>();
  expr->kind = ExprKind::kLogical;
  expr->logical = op;
  expr->children = std::move(children);
  return expr;
}

ExprPtr MakeArithmetic(ArithOp op, ExprPtr left, ExprPtr right) {
  ExprPtr expr = std::make_shared<Expr>();
  expr->kind = ExprKind::kArithmetic;
  expr->arith = op;
  expr->children.push_back(std::move(left));
  expr->children.push_back(std::move(right));
  return expr;
}

ExprPtr MakeLike(ExprPtr child, std::string pattern, bool negated) {
  ExprPtr expr = std::make_shared<Expr>();
  expr->kind = ExprKind::kLike;
  expr->pattern = std::move(pattern);
  expr->negated = negated;
  expr->children.push_back(std::move(child));
  return expr;
}

ExprPtr MakeIsNull(ExprPtr child, bool negated) {
  ExprPtr expr = std::make_shared<Expr>();
  expr->kind = ExprKind::kIsNull;
  expr->negated = negated;
  expr->children.push_back(std::move(child));
  return expr;
}

ExprPtr CloneExpr(const ExprPtr& expr) {
  if (expr == nullptr) {
    return nullptr;
  }
  ExprPtr copy = std::make_shared<Expr>(*expr);
  copy->children.clear();
  for (const ExprPtr& child : expr->children) {
    copy->children.push_back(CloneExpr(child));
  }
  return copy;
}

void BindExpr(Expr& expr, const Schema& schema, int* next_slot) {
  for (ExprPtr& child : expr.children) {
    BindExpr(*child, schema, next_slot);
  }
  switch (expr.kind) {
  case ExprKind::kColumn: {
    int index = schema.IndexOf(expr.column_name);
    if (index < 0 && !expr.table_name.empty()) {
      index = schema.IndexOf(expr.table_name + "." + expr.column_name);
    }
    Check(index >= 0, "unknown column: " + expr.column_name);
    expr.column = static_cast<size_t>(index);
    expr.type = schema.at(expr.column).type;
    expr.scale = schema.at(expr.column).scale;
    break;
  }
  case ExprKind::kConstant:
    expr.type = expr.constant.type;
    expr.scale = expr.constant.scale;
    break;
  case ExprKind::kCompare: {
    for (size_t side = 0; side < 2; ++side) {
      const size_t other = 1 - side;
      if (expr.children[side]->kind == ExprKind::kConstant &&
          expr.children[side]->type == DataType::kString &&
          expr.children[other]->type == DataType::kDate) {
        expr.children[side] = WrapCast(expr.children[side], DataType::kDate, 0);
      }
    }
    const Numeric common = CommonNumeric(expr.children[0]->type, expr.children[0]->scale,
                                         expr.children[1]->type, expr.children[1]->scale);
    expr.children[0] = WrapCast(expr.children[0], common.type, common.scale);
    expr.children[1] = WrapCast(expr.children[1], common.type, common.scale);
    if (expr.children[0]->kind == ExprKind::kCast) {
      BindExpr(*expr.children[0], schema, next_slot);
    }
    if (expr.children[1]->kind == ExprKind::kCast) {
      BindExpr(*expr.children[1], schema, next_slot);
    }
    expr.type = DataType::kInt32;
    break;
  }
  case ExprKind::kLike:
    Check(expr.children[0]->type == DataType::kString, "like needs a text value");
    expr.type = DataType::kInt32;
    break;
  case ExprKind::kLogical:
  case ExprKind::kIsNull:
    expr.type = DataType::kInt32;
    break;
  case ExprKind::kArithmetic: {
    Numeric common = CommonNumeric(expr.children[0]->type, expr.children[0]->scale,
                                   expr.children[1]->type, expr.children[1]->scale);
    if (common.type == DataType::kDate) {
      common.type = DataType::kInt64;
    }
    if (expr.arith == ArithOp::kDivide && common.type == DataType::kDecimal) {
      common = {DataType::kDouble, 0};
    }
    if (expr.arith == ArithOp::kMultiply && common.type == DataType::kDecimal) {
      expr.children[0] = WrapCast(expr.children[0], DataType::kDecimal, expr.children[0]->scale);
      expr.children[1] = WrapCast(expr.children[1], DataType::kDecimal, expr.children[1]->scale);
      expr.type = DataType::kDecimal;
      expr.scale = static_cast<uint8_t>(expr.children[0]->scale + expr.children[1]->scale);
      Check(expr.scale <= 18, "decimal multiplication overflows the supported scale");
      FoldConstants(&expr);
      break;
    }
    expr.children[0] = WrapCast(expr.children[0], common.type, common.scale);
    expr.children[1] = WrapCast(expr.children[1], common.type, common.scale);
    if (expr.children[0]->kind == ExprKind::kCast) {
      BindExpr(*expr.children[0], schema, next_slot);
    }
    if (expr.children[1]->kind == ExprKind::kCast) {
      BindExpr(*expr.children[1], schema, next_slot);
    }
    expr.type = common.type;
    expr.scale = common.scale;
    FoldConstants(&expr);
    break;
  }
  case ExprKind::kCast:
  case ExprKind::kAggregate:
    break;
  }
  if (expr.slot < 0) {
    expr.slot = (*next_slot)++;
  }
}

int CountSlots(const Expr& expr) {
  int total = expr.slot + 1;
  for (const ExprPtr& child : expr.children) {
    const int child_total = CountSlots(*child);
    total = total > child_total ? total : child_total;
  }
  return total;
}

void CollectColumns(const Expr& expr, std::vector<size_t>* columns) {
  if (expr.kind == ExprKind::kColumn) {
    columns->push_back(expr.column);
  }
  for (const ExprPtr& child : expr.children) {
    CollectColumns(*child, columns);
  }
}

std::string DescribeExpr(const Expr& expr, const Schema& schema) {
  switch (expr.kind) {
  case ExprKind::kColumn:
    return expr.column < schema.size() ? schema.at(expr.column).name : expr.column_name;
  case ExprKind::kConstant:
    return FormatValue(expr.constant);
  case ExprKind::kCast:
    return DescribeExpr(*expr.children[0], schema);
  case ExprKind::kLike:
    return DescribeExpr(*expr.children[0], schema) + (expr.negated ? " not like " : " like ") +
           expr.pattern;
  case ExprKind::kIsNull:
    return DescribeExpr(*expr.children[0], schema) + (expr.negated ? " is not null" : " is null");
  case ExprKind::kCompare: {
    const char* op = "=";
    switch (expr.compare) {
    case CompareOp::kEqual:
      op = "=";
      break;
    case CompareOp::kNotEqual:
      op = "<>";
      break;
    case CompareOp::kLess:
      op = "<";
      break;
    case CompareOp::kLessEqual:
      op = "<=";
      break;
    case CompareOp::kGreater:
      op = ">";
      break;
    case CompareOp::kGreaterEqual:
      op = ">=";
      break;
    }
    return DescribeExpr(*expr.children[0], schema) + " " + op + " " +
           DescribeExpr(*expr.children[1], schema);
  }
  case ExprKind::kArithmetic: {
    const char* op = "+";
    switch (expr.arith) {
    case ArithOp::kAdd:
      op = "+";
      break;
    case ArithOp::kSubtract:
      op = "-";
      break;
    case ArithOp::kMultiply:
      op = "*";
      break;
    case ArithOp::kDivide:
      op = "/";
      break;
    }
    return "(" + DescribeExpr(*expr.children[0], schema) + " " + op + " " +
           DescribeExpr(*expr.children[1], schema) + ")";
  }
  case ExprKind::kAggregate:
    return "aggregate";
  case ExprKind::kLogical: {
    if (expr.logical == LogicalOp::kNot) {
      return "not " + DescribeExpr(*expr.children[0], schema);
    }
    const std::string op = expr.logical == LogicalOp::kAnd ? " and " : " or ";
    std::string text = "(";
    for (size_t i = 0; i < expr.children.size(); ++i) {
      text += DescribeExpr(*expr.children[i], schema);
      if (i + 1 < expr.children.size()) {
        text += op;
      }
    }
    return text + ")";
  }
  }
  return "";
}

void ExprContext::Init(int slots, size_t capacity) {
  capacity_ = capacity;
  values_.clear();
  values_.resize(slots < 0 ? 0 : static_cast<size_t>(slots));
  bools_.assign(slots < 0 ? 0 : static_cast<size_t>(slots), std::vector<uint8_t>(capacity, 0));
  selection_a_.assign(capacity, 0);
  selection_b_.assign(capacity, 0);
}

void ExprContext::PrepareSlot(int slot, DataType type, uint8_t scale) {
  Vector& target = values_[slot];
  if (target.capacity() != capacity_ || target.type() != type || target.scale() != scale) {
    target.Init(type, capacity_, scale);
  }
  target.ResetArena();
}

namespace {

inline size_t RowAt(const uint32_t* sel, size_t index) {
  return sel == nullptr ? index : sel[index];
}

void FillConstant(Vector& target, const Value& value, const uint32_t* sel, size_t count) {
  if (target.type() == DataType::kString && !value.is_null) {
    const StringRef text = target.Store(value.text.data(), value.text.size());
    for (size_t i = 0; i < count; ++i) {
      target.strings()[RowAt(sel, i)] = text;
    }
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    target.SetValue(RowAt(sel, i), value);
  }
}

void CastInto(const Vector& source, Vector& target, const uint32_t* sel, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const size_t row = RowAt(sel, i);
    if (!source.validity().Get(row)) {
      target.validity().SetNull(row);
      continue;
    }
    target.SetValue(row, CoerceValue(source.GetValue(row), target.type(), target.scale()));
  }
}

template <typename T>
void ArithmeticLoop(ArithOp op, const T* left, const T* right, T* out, const uint32_t* sel,
                    size_t count, Vector& target) {
  for (size_t i = 0; i < count; ++i) {
    const size_t row = RowAt(sel, i);
    switch (op) {
    case ArithOp::kAdd:
      out[row] = left[row] + right[row];
      break;
    case ArithOp::kSubtract:
      out[row] = left[row] - right[row];
      break;
    case ArithOp::kMultiply:
      out[row] = left[row] * right[row];
      break;
    case ArithOp::kDivide:
      if (right[row] == static_cast<T>(0)) {
        target.validity().SetNull(row);
        out[row] = static_cast<T>(0);
      } else {
        out[row] = left[row] / right[row];
      }
      break;
    }
  }
}

void PropagateValidity(const Vector& source, Vector& target, const uint32_t* sel, size_t count) {
  if (source.validity().all_valid()) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    const size_t row = RowAt(sel, i);
    if (!source.validity().Get(row)) {
      target.validity().SetNull(row);
    }
  }
}

template <typename T>
void CompareLoop(CompareOp op, const T* left, const T* right, const uint32_t* sel, size_t count,
                 uint8_t* out) {
  for (size_t i = 0; i < count; ++i) {
    const size_t row = RowAt(sel, i);
    bool result = false;
    switch (op) {
    case CompareOp::kEqual:
      result = left[row] == right[row];
      break;
    case CompareOp::kNotEqual:
      result = left[row] != right[row];
      break;
    case CompareOp::kLess:
      result = left[row] < right[row];
      break;
    case CompareOp::kLessEqual:
      result = left[row] <= right[row];
      break;
    case CompareOp::kGreater:
      result = left[row] > right[row];
      break;
    case CompareOp::kGreaterEqual:
      result = left[row] >= right[row];
      break;
    }
    out[row] = result ? 1 : 0;
  }
}

void MaskNulls(const Vector& source, const uint32_t* sel, size_t count, uint8_t* out) {
  if (source.validity().all_valid()) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    const size_t row = RowAt(sel, i);
    if (!source.validity().Get(row)) {
      out[row] = 0;
    }
  }
}

}

const Vector& EvalValue(const Expr& expr, const Batch& batch, const uint32_t* sel, size_t count,
                        ExprContext* context) {
  switch (expr.kind) {
  case ExprKind::kColumn:
    return batch.column(expr.column);
  case ExprKind::kConstant: {
    context->PrepareSlot(expr.slot, expr.type, expr.scale);
    Vector& target = context->Slot(expr.slot);
    FillConstant(target, expr.constant, sel, count);
    return target;
  }
  case ExprKind::kCast: {
    const Vector& source = EvalValue(*expr.children[0], batch, sel, count, context);
    context->PrepareSlot(expr.slot, expr.type, expr.scale);
    Vector& target = context->Slot(expr.slot);
    CastInto(source, target, sel, count);
    return target;
  }
  case ExprKind::kArithmetic: {
    const Vector& left = EvalValue(*expr.children[0], batch, sel, count, context);
    const Vector& right = EvalValue(*expr.children[1], batch, sel, count, context);
    context->PrepareSlot(expr.slot, expr.type, expr.scale);
    Vector& target = context->Slot(expr.slot);
    if (expr.type == DataType::kDouble) {
      ArithmeticLoop<double>(expr.arith, left.f64(), right.f64(), target.f64(), sel, count, target);
    } else {
      ArithmeticLoop<int64_t>(expr.arith, left.i64(), right.i64(), target.i64(), sel, count,
                              target);
    }
    PropagateValidity(left, target, sel, count);
    PropagateValidity(right, target, sel, count);
    return target;
  }
  default:
    Fail("expression does not produce a value");
  }
}

void EvalBool(const Expr& expr, const Batch& batch, const uint32_t* sel, size_t count, uint8_t* out,
              ExprContext* context) {
  switch (expr.kind) {
  case ExprKind::kCompare: {
    const Vector& left = EvalValue(*expr.children[0], batch, sel, count, context);
    const Vector& right = EvalValue(*expr.children[1], batch, sel, count, context);
    switch (left.type()) {
    case DataType::kInt32:
    case DataType::kDate:
      CompareLoop<int32_t>(expr.compare, left.i32(), right.i32(), sel, count, out);
      break;
    case DataType::kInt64:
    case DataType::kDecimal:
      CompareLoop<int64_t>(expr.compare, left.i64(), right.i64(), sel, count, out);
      break;
    case DataType::kDouble:
      CompareLoop<double>(expr.compare, left.f64(), right.f64(), sel, count, out);
      break;
    case DataType::kString: {
      for (size_t i = 0; i < count; ++i) {
        const size_t row = RowAt(sel, i);
        const int order = left.strings()[row].Compare(right.strings()[row]);
        bool result = false;
        switch (expr.compare) {
        case CompareOp::kEqual:
          result = order == 0;
          break;
        case CompareOp::kNotEqual:
          result = order != 0;
          break;
        case CompareOp::kLess:
          result = order < 0;
          break;
        case CompareOp::kLessEqual:
          result = order <= 0;
          break;
        case CompareOp::kGreater:
          result = order > 0;
          break;
        case CompareOp::kGreaterEqual:
          result = order >= 0;
          break;
        }
        out[row] = result ? 1 : 0;
      }
      break;
    }
    }
    MaskNulls(left, sel, count, out);
    MaskNulls(right, sel, count, out);
    break;
  }
  case ExprKind::kLogical: {
    if (expr.logical == LogicalOp::kNot) {
      EvalBool(*expr.children[0], batch, sel, count, out, context);
      for (size_t i = 0; i < count; ++i) {
        const size_t row = RowAt(sel, i);
        out[row] = out[row] != 0 ? 0 : 1;
      }
      break;
    }
    EvalBool(*expr.children[0], batch, sel, count, out, context);
    for (size_t child = 1; child < expr.children.size(); ++child) {
      uint8_t* other = context->Bools(expr.children[child]->slot);
      EvalBool(*expr.children[child], batch, sel, count, other, context);
      for (size_t i = 0; i < count; ++i) {
        const size_t row = RowAt(sel, i);
        if (expr.logical == LogicalOp::kAnd) {
          out[row] = (out[row] != 0 && other[row] != 0) ? 1 : 0;
        } else {
          out[row] = (out[row] != 0 || other[row] != 0) ? 1 : 0;
        }
      }
    }
    break;
  }
  case ExprKind::kLike: {
    const Vector& source = EvalValue(*expr.children[0], batch, sel, count, context);
    for (size_t i = 0; i < count; ++i) {
      const size_t row = RowAt(sel, i);
      const StringRef& text = source.strings()[row];
      const bool matched = LikeMatch(text.data, text.size, expr.pattern);
      out[row] = (matched != expr.negated) ? 1 : 0;
    }
    MaskNulls(source, sel, count, out);
    break;
  }
  case ExprKind::kIsNull: {
    const Vector& source = EvalValue(*expr.children[0], batch, sel, count, context);
    for (size_t i = 0; i < count; ++i) {
      const size_t row = RowAt(sel, i);
      const bool is_null = !source.validity().Get(row);
      out[row] = (is_null != expr.negated) ? 1 : 0;
    }
    break;
  }
  default:
    Fail("expression is not a predicate");
  }
}

size_t FilterBatch(const Expr& expr, const Batch& batch, const uint32_t* sel, size_t count,
                   uint32_t* out, ExprContext* context) {
  if (expr.kind == ExprKind::kLogical && expr.logical == LogicalOp::kAnd) {
    const uint32_t* current = sel;
    size_t current_count = count;
    uint32_t* buffers[2] = {context->SelectionA(), context->SelectionB()};
    size_t index = 0;
    for (const ExprPtr& child : expr.children) {
      uint32_t* target = buffers[index % 2];
      current_count = FilterBatch(*child, batch, current, current_count, target, context);
      current = target;
      ++index;
      if (current_count == 0) {
        break;
      }
    }
    for (size_t i = 0; i < current_count; ++i) {
      out[i] = current[i];
    }
    return current_count;
  }

  if (expr.kind == ExprKind::kCompare && expr.children[1]->kind == ExprKind::kConstant &&
      expr.children[0]->kind == ExprKind::kColumn) {
    const Vector& column = batch.column(expr.children[0]->column);
    const Value& constant = expr.children[1]->constant;
    if (!constant.is_null && column.validity().all_valid()) {
      switch (column.type()) {
      case DataType::kInt32:
      case DataType::kDate:
        return SelectInt32(column.i32(), sel, count, expr.compare,
                           static_cast<int32_t>(constant.integer), out);
      case DataType::kInt64:
      case DataType::kDecimal:
        return SelectInt64(column.i64(), sel, count, expr.compare, constant.integer, out);
      case DataType::kDouble:
        return SelectDouble(column.f64(), sel, count, expr.compare, constant.real, out);
      case DataType::kString: {
        const StringRef target{constant.text.data(), static_cast<uint32_t>(constant.text.size())};
        return SelectString(column.strings(), sel, count, expr.compare, target, out);
      }
      }
    }
  }

  uint8_t* flags = context->Bools(expr.slot);
  EvalBool(expr, batch, sel, count, flags, context);
  size_t matched = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t row = static_cast<uint32_t>(RowAt(sel, i));
    if (flags[row] != 0) {
      out[matched++] = row;
    }
  }
  return matched;
}

Value EvalRowValue(const Expr& expr, const std::vector<Value>& row) {
  switch (expr.kind) {
  case ExprKind::kColumn:
    return row[expr.column];
  case ExprKind::kConstant:
    return expr.constant;
  case ExprKind::kCast: {
    const Value source = EvalRowValue(*expr.children[0], row);
    if (source.is_null) {
      return Value::Null(expr.type);
    }
    return CoerceValue(source, expr.type, expr.scale);
  }
  case ExprKind::kArithmetic: {
    const Value left = EvalRowValue(*expr.children[0], row);
    const Value right = EvalRowValue(*expr.children[1], row);
    if (left.is_null || right.is_null) {
      return Value::Null(expr.type);
    }
    if (expr.type == DataType::kDouble) {
      const double a = left.real;
      const double b = right.real;
      switch (expr.arith) {
      case ArithOp::kAdd:
        return Value::Real(a + b);
      case ArithOp::kSubtract:
        return Value::Real(a - b);
      case ArithOp::kMultiply:
        return Value::Real(a * b);
      case ArithOp::kDivide:
        return b == 0.0 ? Value::Null(expr.type) : Value::Real(a / b);
      }
    }
    const int64_t a = left.integer;
    const int64_t b = right.integer;
    int64_t result = 0;
    switch (expr.arith) {
    case ArithOp::kAdd:
      result = a + b;
      break;
    case ArithOp::kSubtract:
      result = a - b;
      break;
    case ArithOp::kMultiply:
      result = a * b;
      break;
    case ArithOp::kDivide:
      if (b == 0) {
        return Value::Null(expr.type);
      }
      result = a / b;
      break;
    }
    if (expr.type == DataType::kDecimal) {
      return Value::Decimal(result, expr.scale);
    }
    return Value::Int(result, expr.type);
  }
  default:
    Fail("expression does not produce a value");
  }
}

bool EvalRowBool(const Expr& expr, const std::vector<Value>& row) {
  switch (expr.kind) {
  case ExprKind::kCompare: {
    const Value left = EvalRowValue(*expr.children[0], row);
    const Value right = EvalRowValue(*expr.children[1], row);
    if (left.is_null || right.is_null) {
      return false;
    }
    int order = 0;
    if (left.type == DataType::kString) {
      order = left.text.compare(right.text);
      order = order < 0 ? -1 : (order > 0 ? 1 : 0);
    } else if (left.type == DataType::kDouble) {
      order = left.real < right.real ? -1 : (left.real > right.real ? 1 : 0);
    } else {
      order = left.integer < right.integer ? -1 : (left.integer > right.integer ? 1 : 0);
    }
    switch (expr.compare) {
    case CompareOp::kEqual:
      return order == 0;
    case CompareOp::kNotEqual:
      return order != 0;
    case CompareOp::kLess:
      return order < 0;
    case CompareOp::kLessEqual:
      return order <= 0;
    case CompareOp::kGreater:
      return order > 0;
    case CompareOp::kGreaterEqual:
      return order >= 0;
    }
    return false;
  }
  case ExprKind::kLogical: {
    if (expr.logical == LogicalOp::kNot) {
      return !EvalRowBool(*expr.children[0], row);
    }
    if (expr.logical == LogicalOp::kAnd) {
      for (const ExprPtr& child : expr.children) {
        if (!EvalRowBool(*child, row)) {
          return false;
        }
      }
      return true;
    }
    for (const ExprPtr& child : expr.children) {
      if (EvalRowBool(*child, row)) {
        return true;
      }
    }
    return false;
  }
  case ExprKind::kLike: {
    const Value source = EvalRowValue(*expr.children[0], row);
    if (source.is_null) {
      return false;
    }
    const bool matched = LikeMatch(source.text.data(), source.text.size(), expr.pattern);
    return matched != expr.negated;
  }
  case ExprKind::kIsNull: {
    const Value source = EvalRowValue(*expr.children[0], row);
    return source.is_null != expr.negated;
  }
  default:
    Fail("expression is not a predicate");
  }
}

}
