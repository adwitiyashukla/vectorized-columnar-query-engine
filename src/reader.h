#pragma once

#include <memory>
#include <string>
#include <vector>

#include "format.h"
#include "util.h"
#include "vector.h"

namespace vcqe {

struct ColumnChunk {
  DataType type = DataType::kInt64;
  uint8_t scale = 0;
  size_t count = 0;
  std::vector<uint8_t> fixed;
  std::vector<StringRef> strings;
  std::vector<char> blob;
  std::vector<uint64_t> validity;
  bool all_valid = true;

  const int32_t* i32() const {
    return reinterpret_cast<const int32_t*>(fixed.data());
  }
  const int64_t* i64() const {
    return reinterpret_cast<const int64_t*>(fixed.data());
  }
  const double* f64() const {
    return reinterpret_cast<const double*>(fixed.data());
  }
  bool IsValid(size_t index) const {
    return all_valid || ((validity[index >> 6] >> (index & 63)) & 1ull) != 0;
  }
};

class TableReader {
public:
  explicit TableReader(const std::string& path);

  const Schema& schema() const {
    return meta_.schema;
  }
  const TableMeta& meta() const {
    return meta_;
  }
  uint64_t rows() const {
    return meta_.rows;
  }
  size_t row_group_count() const {
    return meta_.row_groups.size();
  }
  uint64_t RowGroupRows(size_t group) const {
    return meta_.row_groups.at(group).rows;
  }
  const ChunkMeta& Chunk(size_t group, size_t column) const {
    return meta_.row_groups.at(group).chunks.at(column);
  }
  const std::string& path() const {
    return path_;
  }

private:
  std::string path_;
  TableMeta meta_;
};

class ChunkCursor {
public:
  ChunkCursor(const TableReader& table);
  void Read(size_t group, size_t column, ColumnChunk* out);

private:
  const TableReader& table_;
  FileReader file_;
  std::vector<uint8_t> scratch_;
};

}
