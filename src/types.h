#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vcqe {

enum class DataType : uint8_t {
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
  kDecimal = 4,
  kDate = 5,
  kString = 6,
};

const char* TypeName(DataType type);
DataType TypeFromName(const std::string& name);
bool IsFixedWidth(DataType type);
size_t TypeWidth(DataType type);

struct ColumnSchema {
  std::string name;
  DataType type = DataType::kInt64;
  uint8_t scale = 0;
};

class Schema {
public:
  Schema() = default;
  explicit Schema(std::vector<ColumnSchema> columns) : columns_(std::move(columns)) {}

  const std::vector<ColumnSchema>& columns() const {
    return columns_;
  }
  size_t size() const {
    return columns_.size();
  }
  const ColumnSchema& at(size_t index) const {
    return columns_.at(index);
  }
  void Add(ColumnSchema column) {
    columns_.push_back(std::move(column));
  }
  int IndexOf(const std::string& name) const;
  size_t RequireIndex(const std::string& name) const;

private:
  std::vector<ColumnSchema> columns_;
};

struct Value {
  DataType type = DataType::kInt64;
  bool is_null = true;
  int64_t integer = 0;
  double real = 0.0;
  std::string text;

  static Value Null(DataType type);
  static Value Int(int64_t value, DataType type = DataType::kInt64);
  static Value Decimal(int64_t units, uint8_t scale);
  static Value Real(double value);
  static Value Date(int32_t days);
  static Value String(std::string value);

  uint8_t scale = 0;
};

int32_t DaysFromCivil(int year, unsigned month, unsigned day);
void CivilFromDays(int32_t days, int* year, unsigned* month, unsigned* day);
int32_t ParseDate(const std::string& text);
std::string FormatDate(int32_t days);
int64_t ParseDecimal(const std::string& text, uint8_t scale);
std::string FormatDecimal(int64_t units, uint8_t scale);
std::string FormatValue(const Value& value);

}
