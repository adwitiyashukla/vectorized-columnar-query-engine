#include "csv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "error.h"
#include "util.h"
#include "writer.h"

namespace vcqe {
namespace {

int64_t ParseInteger(const char* text, size_t size) {
  int64_t value = 0;
  size_t position = 0;
  bool negative = false;
  if (position < size && (text[position] == '-' || text[position] == '+')) {
    negative = text[position] == '-';
    ++position;
  }
  Check(position < size, "empty number in the input file");
  for (; position < size; ++position) {
    Check(text[position] >= '0' && text[position] <= '9',
          "bad number in the input file: " + std::string(text, size));
    value = value * 10 + (text[position] - '0');
  }
  return negative ? -value : value;
}

}

Schema ParseSchemaText(const std::string& text) {
  Schema schema;
  for (const std::string& part : Split(text, ',')) {
    const std::string trimmed = Trim(part);
    if (trimmed.empty()) {
      continue;
    }
    const std::vector<std::string> pieces = Split(trimmed, ':');
    Check(pieces.size() == 2 || pieces.size() == 3, "schema entries look like name:type[:scale]");
    ColumnSchema column;
    column.name = ToLower(Trim(pieces[0]));
    column.type = TypeFromName(ToLower(Trim(pieces[1])));
    if (pieces.size() == 3) {
      column.scale = static_cast<uint8_t>(std::stoi(Trim(pieces[2])));
    } else if (column.type == DataType::kDecimal) {
      column.scale = 2;
    }
    schema.Add(std::move(column));
  }
  Check(schema.size() > 0, "schema is empty");
  return schema;
}

LoadReport LoadDelimited(const std::string& source, const std::string& target, const Schema& schema,
                         const Config& config) {
  Timer timer;
  std::ifstream input(source, std::ios::binary);
  Check(input.good(), "cannot open input file: " + source);
  input.seekg(0, std::ios::end);
  const uint64_t source_bytes = static_cast<uint64_t>(input.tellg());
  input.seekg(0, std::ios::beg);

  TableWriter writer(target, schema, config.storage);
  const char delimiter = config.csv.delimiter;
  const size_t block_size = 8u << 20;
  std::vector<char> buffer(block_size);
  std::string carry;
  uint64_t rows = 0;
  bool skip_header = config.csv.has_header;

  auto emit = [&](const char* line, size_t size) {
    if (size > 0 && line[size - 1] == '\r') {
      --size;
    }
    if (size == 0) {
      return;
    }
    if (skip_header) {
      skip_header = false;
      return;
    }
    size_t column = 0;
    size_t begin = 0;
    for (size_t i = 0; i <= size; ++i) {
      if (i != size && line[i] != delimiter) {
        continue;
      }
      if (i == size && column == schema.size() && begin == size) {
        break;
      }
      Check(column < schema.size(), "row has more columns than the schema");
      const char* field = line + begin;
      const size_t length = i - begin;
      const ColumnSchema& target_column = schema.at(column);
      const bool is_null = length == config.csv.null_string.size() &&
                           std::memcmp(field, config.csv.null_string.data(), length) == 0 &&
                           (target_column.type != DataType::kString || length > 0);
      if (is_null) {
        writer.AppendNull(column);
      } else {
        switch (target_column.type) {
        case DataType::kInt32:
        case DataType::kInt64:
          writer.AppendInt(column, ParseInteger(field, length));
          break;
        case DataType::kDecimal:
          writer.AppendInt(column, ParseDecimal(std::string(field, length), target_column.scale));
          break;
        case DataType::kDate:
          writer.AppendInt(column, ParseDate(std::string(field, length)));
          break;
        case DataType::kDouble:
          writer.AppendReal(column, std::strtod(std::string(field, length).c_str(), nullptr));
          break;
        case DataType::kString:
          writer.AppendText(column, field, length);
          break;
        }
      }
      ++column;
      begin = i + 1;
    }
    Check(column == schema.size(), "row has fewer columns than the schema");
    writer.EndRow();
    ++rows;
  };

  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const size_t filled = static_cast<size_t>(input.gcount());
    if (filled == 0) {
      break;
    }
    size_t start = 0;
    for (size_t i = 0; i < filled; ++i) {
      if (buffer[i] != '\n') {
        continue;
      }
      if (carry.empty()) {
        emit(buffer.data() + start, i - start);
      } else {
        carry.append(buffer.data() + start, i - start);
        emit(carry.data(), carry.size());
        carry.clear();
      }
      start = i + 1;
    }
    carry.append(buffer.data() + start, filled - start);
  }
  if (!carry.empty()) {
    emit(carry.data(), carry.size());
  }
  writer.Finish();

  LoadReport report;
  report.rows = rows;
  report.source_bytes = source_bytes;
  report.seconds = timer.Seconds();
  std::ifstream written(target, std::ios::binary | std::ios::ate);
  report.target_bytes = written.good() ? static_cast<uint64_t>(written.tellg()) : 0;
  return report;
}

}
