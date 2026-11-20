#pragma once

#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "format.h"
#include "util.h"

namespace vcqe {

class TableWriter {
public:
  TableWriter(const std::string& path, Schema schema, const StorageConfig& config);
  ~TableWriter();

  void AppendInt(size_t column, int64_t value);
  void AppendReal(size_t column, double value);
  void AppendText(size_t column, const char* data, size_t size);
  void AppendNull(size_t column);
  void EndRow();
  void Finish();

  uint64_t rows() const {
    return meta_.rows;
  }

private:
  struct ColumnBuffer {
    std::vector<int64_t> integers;
    std::vector<double> reals;
    std::vector<char> blob;
    std::vector<uint32_t> offsets;
    std::vector<uint8_t> valid;
    uint64_t nulls = 0;
  };

  void FlushRowGroup();
  ChunkMeta WriteChunk(size_t column, size_t rows);
  std::vector<uint8_t> EncodeFixed(size_t column, size_t rows, const std::vector<uint8_t>& validity,
                                   Encoding* encoding, ZoneMap* zone, uint64_t* distinct);
  std::vector<uint8_t> EncodeText(size_t column, size_t rows, const std::vector<uint8_t>& validity,
                                  Encoding* encoding, ZoneMap* zone, uint64_t* distinct);

  FileWriter file_;
  StorageConfig config_;
  TableMeta meta_;
  std::vector<ColumnBuffer> buffers_;
  size_t staged_rows_ = 0;
  bool finished_ = false;
  bool broken_ = false;
};

}
