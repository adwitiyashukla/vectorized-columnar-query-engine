#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "error.h"
#include "types.h"

namespace vcqe {

struct StringRef {
  const char* data = nullptr;
  uint32_t size = 0;

  std::string ToString() const {
    return std::string(data, data + size);
  }
  bool Equals(const StringRef& other) const {
    return size == other.size && std::memcmp(data, other.data, size) == 0;
  }
  int Compare(const StringRef& other) const {
    const uint32_t shortest = size < other.size ? size : other.size;
    const int order = shortest == 0 ? 0 : std::memcmp(data, other.data, shortest);
    if (order != 0) {
      return order;
    }
    if (size == other.size) {
      return 0;
    }
    return size < other.size ? -1 : 1;
  }
};

class Bitmap {
public:
  void Reset(size_t count) {
    count_ = count;
    all_valid_ = true;
    words_.clear();
  }
  bool all_valid() const {
    return all_valid_;
  }
  bool Get(size_t index) const {
    if (all_valid_) {
      return true;
    }
    return (words_[index >> 6] >> (index & 63)) & 1ull;
  }
  void SetNull(size_t index) {
    Materialize();
    words_[index >> 6] &= ~(1ull << (index & 63));
  }
  void SetValid(size_t index) {
    if (all_valid_) {
      return;
    }
    words_[index >> 6] |= 1ull << (index & 63);
  }
  void Materialize() {
    if (!all_valid_) {
      return;
    }
    words_.assign((count_ + 63) / 64, ~0ull);
    all_valid_ = false;
  }
  const uint64_t* words() const {
    return words_.data();
  }

private:
  std::vector<uint64_t> words_;
  size_t count_ = 0;
  bool all_valid_ = true;
};

class Vector {
public:
  Vector() = default;

  void Init(DataType type, size_t capacity, uint8_t scale = 0) {
    type_ = type;
    scale_ = scale;
    capacity_ = capacity;
    if (type == DataType::kString) {
      strings_.assign(capacity, StringRef());
    } else {
      bytes_.assign(capacity * TypeWidth(type), 0);
    }
    validity_.Reset(capacity);
  }

  DataType type() const {
    return type_;
  }
  uint8_t scale() const {
    return scale_;
  }
  void set_scale(uint8_t scale) {
    scale_ = scale;
  }
  size_t capacity() const {
    return capacity_;
  }

  int32_t* i32() {
    return reinterpret_cast<int32_t*>(bytes_.data());
  }
  const int32_t* i32() const {
    return reinterpret_cast<const int32_t*>(bytes_.data());
  }
  int64_t* i64() {
    return reinterpret_cast<int64_t*>(bytes_.data());
  }
  const int64_t* i64() const {
    return reinterpret_cast<const int64_t*>(bytes_.data());
  }
  double* f64() {
    return reinterpret_cast<double*>(bytes_.data());
  }
  const double* f64() const {
    return reinterpret_cast<const double*>(bytes_.data());
  }
  StringRef* strings() {
    return strings_.data();
  }
  const StringRef* strings() const {
    return strings_.data();
  }
  uint8_t* bytes() {
    return bytes_.data();
  }

  Bitmap& validity() {
    return validity_;
  }
  const Bitmap& validity() const {
    return validity_;
  }

  std::vector<char>& arena() {
    return arena_;
  }
  void ResetArena() {
    arena_.clear();
  }
  void ResetValidity() {
    validity_.Reset(capacity_);
  }
  void PrepareStrings(size_t count) {
    arena_.clear();
    offsets_.assign(count, 0);
  }
  void AppendString(size_t index, const char* text, uint32_t size) {
    offsets_[index] = static_cast<uint32_t>(arena_.size());
    arena_.insert(arena_.end(), text, text + size);
    strings_[index].size = size;
  }
  void FinishStrings(size_t count) {
    for (size_t i = 0; i < count; ++i) {
      strings_[i].data = arena_.data() + offsets_[i];
    }
  }
  StringRef Store(const char* text, size_t size) {
    const size_t offset = arena_.size();
    arena_.insert(arena_.end(), text, text + size);
    return StringRef{arena_.data() + offset, static_cast<uint32_t>(size)};
  }

  int64_t GetInteger(size_t index) const {
    switch (type_) {
    case DataType::kInt32:
    case DataType::kDate:
      return i32()[index];
    default:
      return i64()[index];
    }
  }

  Value GetValue(size_t index) const {
    if (!validity_.Get(index)) {
      return Value::Null(type_);
    }
    switch (type_) {
    case DataType::kInt32:
      return Value::Int(i32()[index], DataType::kInt32);
    case DataType::kDate:
      return Value::Date(i32()[index]);
    case DataType::kInt64:
      return Value::Int(i64()[index], DataType::kInt64);
    case DataType::kDecimal:
      return Value::Decimal(i64()[index], scale_);
    case DataType::kDouble:
      return Value::Real(f64()[index]);
    case DataType::kString:
      return Value::String(strings_[index].ToString());
    }
    return Value::Null(type_);
  }

  void SetValue(size_t index, const Value& value) {
    if (value.is_null) {
      validity_.SetNull(index);
      return;
    }
    switch (type_) {
    case DataType::kInt32:
    case DataType::kDate:
      i32()[index] = static_cast<int32_t>(value.integer);
      break;
    case DataType::kInt64:
    case DataType::kDecimal:
      i64()[index] = value.integer;
      break;
    case DataType::kDouble:
      f64()[index] = value.real;
      break;
    case DataType::kString:
      strings_[index] = Store(value.text.data(), value.text.size());
      break;
    }
  }

private:
  DataType type_ = DataType::kInt64;
  uint8_t scale_ = 0;
  size_t capacity_ = 0;
  std::vector<uint8_t> bytes_;
  std::vector<StringRef> strings_;
  std::vector<char> arena_;
  std::vector<uint32_t> offsets_;
  Bitmap validity_;
};

class Batch {
public:
  void Init(const std::vector<ColumnSchema>& columns, size_t capacity) {
    columns_.clear();
    columns_.resize(columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
      columns_[i].Init(columns[i].type, capacity, columns[i].scale);
    }
    capacity_ = capacity;
    selection_.assign(capacity, 0);
    has_selection_ = false;
    count_ = 0;
  }

  size_t width() const {
    return columns_.size();
  }
  size_t capacity() const {
    return capacity_;
  }
  size_t count() const {
    return count_;
  }
  void set_count(size_t count) {
    count_ = count;
  }

  Vector& column(size_t index) {
    return columns_[index];
  }
  const Vector& column(size_t index) const {
    return columns_[index];
  }

  bool has_selection() const {
    return has_selection_;
  }
  uint32_t* selection() {
    return selection_.data();
  }
  const uint32_t* selection() const {
    return selection_.data();
  }
  const uint32_t* selection_or_null() const {
    return has_selection_ ? selection_.data() : nullptr;
  }
  void set_has_selection(bool value) {
    has_selection_ = value;
  }
  size_t RowIndex(size_t position) const {
    return has_selection_ ? selection_[position] : position;
  }

  void ResetSelection() {
    has_selection_ = false;
    count_ = 0;
  }

private:
  std::vector<Vector> columns_;
  std::vector<uint32_t> selection_;
  size_t capacity_ = 0;
  size_t count_ = 0;
  bool has_selection_ = false;
};

}
