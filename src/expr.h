#pragma once

#include <memory>
#include <string>
#include <vector>

#include "kernels.h"
#include "types.h"
#include "vector.h"

namespace vcqe {

enum class ExprKind : uint8_t {
  kColumn,
  kConstant,
  kCompare,
  kLogical,
  kArithmetic,
  kCast,
  kLike,
  kIsNull,
  kAggregate,
};

enum class LogicalOp : uint8_t { kAnd, kOr, kNot };
enum class ArithOp : uint8_t { kAdd, kSubtract, kMultiply, kDivide };

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::kColumn;
  DataType type = DataType::kInt64;
  uint8_t scale = 0;
  size_t column = 0;
  std::string column_name;
  std::string table_name;
  Value constant;
  CompareOp compare = CompareOp::kEqual;
  LogicalOp logical = LogicalOp::kAnd;
  ArithOp arith = ArithOp::kAdd;
  std::string pattern;
  bool negated = false;
  int slot = -1;
  uint8_t aggregate = 0;
  bool star = false;
  std::vector<ExprPtr> children;
};

ExprPtr MakeColumn(const std::string& table, const std::string& name);
ExprPtr MakeConstant(Value value);
ExprPtr MakeCompare(CompareOp op, ExprPtr left, ExprPtr right);
ExprPtr MakeLogical(LogicalOp op, std::vector<ExprPtr> children);
ExprPtr MakeArithmetic(ArithOp op, ExprPtr left, ExprPtr right);
ExprPtr MakeLike(ExprPtr child, std::string pattern, bool negated);
ExprPtr MakeIsNull(ExprPtr child, bool negated);

void BindExpr(Expr& expr, const Schema& schema, int* next_slot);
int CountSlots(const Expr& expr);
void CollectColumns(const Expr& expr, std::vector<size_t>* columns);
ExprPtr CloneExpr(const ExprPtr& expr);
std::string DescribeExpr(const Expr& expr, const Schema& schema);

class ExprContext {
public:
  void Init(int slots, size_t capacity);
  Vector& Slot(int slot) {
    return values_[slot];
  }
  uint8_t* Bools(int slot) {
    return bools_[slot].data();
  }
  uint32_t* SelectionA() {
    return selection_a_.data();
  }
  uint32_t* SelectionB() {
    return selection_b_.data();
  }
  size_t capacity() const {
    return capacity_;
  }
  void PrepareSlot(int slot, DataType type, uint8_t scale);

private:
  std::vector<Vector> values_;
  std::vector<std::vector<uint8_t>> bools_;
  std::vector<uint32_t> selection_a_;
  std::vector<uint32_t> selection_b_;
  size_t capacity_ = 0;
};

const Vector& EvalValue(const Expr& expr, const Batch& batch, const uint32_t* sel, size_t count,
                        ExprContext* context);
void EvalBool(const Expr& expr, const Batch& batch, const uint32_t* sel, size_t count, uint8_t* out,
              ExprContext* context);
size_t FilterBatch(const Expr& expr, const Batch& batch, const uint32_t* sel, size_t count,
                   uint32_t* out, ExprContext* context);

Value EvalRowValue(const Expr& expr, const std::vector<Value>& row);
bool EvalRowBool(const Expr& expr, const std::vector<Value>& row);

}
