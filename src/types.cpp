#include "types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "error.h"

namespace vcqe {

const char* TypeName(DataType type) {
  switch (type) {
  case DataType::kInt32:
    return "int32";
  case DataType::kInt64:
    return "int64";
  case DataType::kDouble:
    return "double";
  case DataType::kDecimal:
    return "decimal";
  case DataType::kDate:
    return "date";
  case DataType::kString:
    return "string";
  }
  return "unknown";
}

DataType TypeFromName(const std::string& name) {
  if (name == "int32")
    return DataType::kInt32;
  if (name == "int64")
    return DataType::kInt64;
  if (name == "double")
    return DataType::kDouble;
  if (name == "decimal")
    return DataType::kDecimal;
  if (name == "date")
    return DataType::kDate;
  if (name == "string")
    return DataType::kString;
  Fail("unknown type name: " + name);
}

bool IsFixedWidth(DataType type) {
  return type != DataType::kString;
}

size_t TypeWidth(DataType type) {
  switch (type) {
  case DataType::kInt32:
  case DataType::kDate:
    return 4;
  case DataType::kInt64:
  case DataType::kDecimal:
  case DataType::kDouble:
    return 8;
  case DataType::kString:
    return 0;
  }
  return 0;
}

int Schema::IndexOf(const std::string& name) const {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

size_t Schema::RequireIndex(const std::string& name) const {
  int index = IndexOf(name);
  Check(index >= 0, "unknown column: " + name);
  return static_cast<size_t>(index);
}

Value Value::Null(DataType type) {
  Value value;
  value.type = type;
  value.is_null = true;
  return value;
}

Value Value::Int(int64_t number, DataType type) {
  Value value;
  value.type = type;
  value.is_null = false;
  value.integer = number;
  return value;
}

Value Value::Decimal(int64_t units, uint8_t scale) {
  Value value;
  value.type = DataType::kDecimal;
  value.is_null = false;
  value.integer = units;
  value.scale = scale;
  return value;
}

Value Value::Real(double number) {
  Value value;
  value.type = DataType::kDouble;
  value.is_null = false;
  value.real = number;
  return value;
}

Value Value::Date(int32_t days) {
  Value value;
  value.type = DataType::kDate;
  value.is_null = false;
  value.integer = days;
  return value;
}

Value Value::String(std::string text) {
  Value value;
  value.type = DataType::kString;
  value.is_null = false;
  value.text = std::move(text);
  return value;
}

int32_t DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<int32_t>(era * 146097 + static_cast<int>(day_of_era) - 719468);
}

void CivilFromDays(int32_t days, int* year, unsigned* month, unsigned* day) {
  int32_t shifted = days + 719468;
  const int era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(shifted - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const int computed_year = static_cast<int>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned mp = (5 * day_of_year + 2) / 153;
  const unsigned computed_day = day_of_year - (153 * mp + 2) / 5 + 1;
  const unsigned computed_month = mp + (mp < 10 ? 3 : -9);
  *year = computed_year + (computed_month <= 2);
  *month = computed_month;
  *day = computed_day;
}

int32_t ParseDate(const std::string& text) {
  Check(text.size() == 10 && text[4] == '-' && text[7] == '-', "bad date literal: " + text);
  for (size_t i = 0; i < text.size(); ++i) {
    if (i == 4 || i == 7) {
      continue;
    }
    Check(text[i] >= '0' && text[i] <= '9', "bad date literal: " + text);
  }
  int year = std::stoi(text.substr(0, 4));
  unsigned month = static_cast<unsigned>(std::stoi(text.substr(5, 2)));
  unsigned day = static_cast<unsigned>(std::stoi(text.substr(8, 2)));
  Check(month >= 1 && month <= 12, "bad month in date: " + text);
  Check(day >= 1 && day <= 31, "bad day in date: " + text);
  return DaysFromCivil(year, month, day);
}

std::string FormatDate(int32_t days) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CivilFromDays(days, &year, &month, &day);
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", year, month, day);
  return std::string(buffer);
}

int64_t ParseDecimal(const std::string& text, uint8_t scale) {
  size_t position = 0;
  bool negative = false;
  if (position < text.size() && (text[position] == '-' || text[position] == '+')) {
    negative = text[position] == '-';
    ++position;
  }
  int64_t units = 0;
  bool any_digit = false;
  while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
    units = units * 10 + (text[position] - '0');
    any_digit = true;
    ++position;
  }
  int64_t fraction = 0;
  int seen = 0;
  if (position < text.size() && text[position] == '.') {
    ++position;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      if (seen < scale) {
        fraction = fraction * 10 + (text[position] - '0');
        ++seen;
      }
      any_digit = true;
      ++position;
    }
  }
  Check(any_digit && position == text.size(), "bad decimal literal: " + text);
  for (; seen < scale; ++seen) {
    fraction *= 10;
  }
  int64_t factor = 1;
  for (uint8_t i = 0; i < scale; ++i) {
    factor *= 10;
  }
  int64_t result = units * factor + fraction;
  return negative ? -result : result;
}

std::string FormatDecimal(int64_t units, uint8_t scale) {
  if (scale == 0) {
    return std::to_string(units);
  }
  int64_t factor = 1;
  for (uint8_t i = 0; i < scale; ++i) {
    factor *= 10;
  }
  bool negative = units < 0;
  uint64_t magnitude =
      negative ? static_cast<uint64_t>(-(units + 1)) + 1 : static_cast<uint64_t>(units);
  uint64_t whole = magnitude / static_cast<uint64_t>(factor);
  uint64_t rest = magnitude % static_cast<uint64_t>(factor);
  std::string digits = std::to_string(rest);
  while (digits.size() < scale) {
    digits.insert(digits.begin(), '0');
  }
  return (negative ? "-" : "") + std::to_string(whole) + "." + digits;
}

std::string FormatValue(const Value& value) {
  if (value.is_null) {
    return "NULL";
  }
  switch (value.type) {
  case DataType::kInt32:
  case DataType::kInt64:
    return std::to_string(value.integer);
  case DataType::kDecimal:
    return FormatDecimal(value.integer, value.scale);
  case DataType::kDate:
    return FormatDate(static_cast<int32_t>(value.integer));
  case DataType::kDouble: {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value.real);
    return std::string(buffer);
  }
  case DataType::kString:
    return value.text;
  }
  return "";
}

}
