#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "error.h"
#include "types.h"

namespace vcqe {

constexpr char kMagic[8] = {'V', 'C', 'Q', 'E', '0', '0', '0', '1'};
constexpr uint32_t kFormatVersion = 1;

enum class Encoding : uint8_t {
  kPlain = 1,
  kBitPacked = 2,
  kDictionary = 3,
  kPlainString = 4,
};

struct ZoneMap {
  bool valid = false;
  int64_t min_integer = 0;
  int64_t max_integer = 0;
  double min_real = 0.0;
  double max_real = 0.0;
  std::string min_text;
  std::string max_text;
};

struct ChunkMeta {
  Encoding encoding = Encoding::kPlain;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t crc = 0;
  uint64_t count = 0;
  uint64_t null_count = 0;
  uint64_t distinct_count = 0;
  ZoneMap zone;
};

struct RowGroupMeta {
  uint64_t rows = 0;
  std::vector<ChunkMeta> chunks;
};

struct TableMeta {
  Schema schema;
  uint64_t rows = 0;
  std::vector<RowGroupMeta> row_groups;
};

class ByteWriter {
public:
  void U8(uint8_t value) {
    buffer_.push_back(value);
  }
  void U32(uint32_t value) {
    Raw(&value, sizeof(value));
  }
  void U64(uint64_t value) {
    Raw(&value, sizeof(value));
  }
  void I64(int64_t value) {
    Raw(&value, sizeof(value));
  }
  void F64(double value) {
    Raw(&value, sizeof(value));
  }
  void Text(const std::string& value) {
    U32(static_cast<uint32_t>(value.size()));
    Raw(value.data(), value.size());
  }
  void Raw(const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
  }
  const std::vector<uint8_t>& buffer() const {
    return buffer_;
  }
  std::vector<uint8_t>& buffer() {
    return buffer_;
  }

private:
  std::vector<uint8_t> buffer_;
};

class ByteReader {
public:
  ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  uint8_t U8() {
    Require(1);
    return data_[position_++];
  }
  uint32_t U32() {
    uint32_t value = 0;
    Raw(&value, sizeof(value));
    return value;
  }
  uint64_t U64() {
    uint64_t value = 0;
    Raw(&value, sizeof(value));
    return value;
  }
  int64_t I64() {
    int64_t value = 0;
    Raw(&value, sizeof(value));
    return value;
  }
  double F64() {
    double value = 0.0;
    Raw(&value, sizeof(value));
    return value;
  }
  std::string Text() {
    const uint32_t length = U32();
    Require(length);
    std::string value(reinterpret_cast<const char*>(data_ + position_), length);
    position_ += length;
    return value;
  }
  void Raw(void* out, size_t size) {
    Require(size);
    std::memcpy(out, data_ + position_, size);
    position_ += size;
  }
  const uint8_t* Take(size_t size) {
    Require(size);
    const uint8_t* pointer = data_ + position_;
    position_ += size;
    return pointer;
  }
  size_t remaining() const {
    return size_ - position_;
  }
  size_t position() const {
    return position_;
  }

private:
  void Require(size_t size) {
    Check(position_ + size <= size_ && position_ + size >= position_,
          "truncated vcqe file metadata");
  }

  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
};

void EncodeMeta(const TableMeta& meta, ByteWriter* out);
TableMeta DecodeMeta(const uint8_t* data, size_t size);

}
