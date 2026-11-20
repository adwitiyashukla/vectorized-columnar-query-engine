#include "writer.h"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_set>

#include "kernels.h"

namespace vcqe {
namespace {

constexpr size_t kDistinctCap = 8192;

uint64_t CountDistinctInteger(const std::vector<int64_t>& values, const std::vector<uint8_t>& valid,
                              size_t rows, int64_t smallest, int64_t largest) {
  const uint64_t range = static_cast<uint64_t>(largest) - static_cast<uint64_t>(smallest);
  if (range < (1u << 20)) {
    std::vector<bool> seen(range + 1, false);
    uint64_t distinct = 0;
    for (size_t i = 0; i < rows; ++i) {
      if (valid[i] == 0) {
        continue;
      }
      const uint64_t offset = static_cast<uint64_t>(values[i]) - static_cast<uint64_t>(smallest);
      if (!seen[offset]) {
        seen[offset] = true;
        ++distinct;
      }
    }
    return distinct;
  }
  std::unordered_set<int64_t> seen;
  for (size_t i = 0; i < rows; ++i) {
    if (valid[i] == 0) {
      continue;
    }
    seen.insert(values[i]);
    if (seen.size() >= kDistinctCap) {
      return rows;
    }
  }
  return seen.size();
}

uint64_t CountDistinctReal(const std::vector<double>& values, const std::vector<uint8_t>& valid,
                           size_t rows) {
  std::unordered_set<double> seen;
  for (size_t i = 0; i < rows; ++i) {
    if (valid[i] == 0) {
      continue;
    }
    seen.insert(values[i]);
    if (seen.size() >= kDistinctCap) {
      return rows;
    }
  }
  return seen.size();
}

void EncodeZone(const ZoneMap& zone, DataType type, ByteWriter* out) {
  out->U8(zone.valid ? 1 : 0);
  if (!zone.valid) {
    return;
  }
  if (type == DataType::kString) {
    out->Text(zone.min_text);
    out->Text(zone.max_text);
  } else if (type == DataType::kDouble) {
    out->F64(zone.min_real);
    out->F64(zone.max_real);
  } else {
    out->I64(zone.min_integer);
    out->I64(zone.max_integer);
  }
}

}

void EncodeMeta(const TableMeta& meta, ByteWriter* out) {
  out->U32(kFormatVersion);
  out->U64(meta.rows);
  out->U32(static_cast<uint32_t>(meta.schema.size()));
  for (const ColumnSchema& column : meta.schema.columns()) {
    out->Text(column.name);
    out->U8(static_cast<uint8_t>(column.type));
    out->U8(column.scale);
  }
  out->U32(static_cast<uint32_t>(meta.row_groups.size()));
  for (const RowGroupMeta& group : meta.row_groups) {
    out->U64(group.rows);
    for (size_t i = 0; i < group.chunks.size(); ++i) {
      const ChunkMeta& chunk = group.chunks[i];
      out->U8(static_cast<uint8_t>(chunk.encoding));
      out->U64(chunk.offset);
      out->U64(chunk.size);
      out->U32(chunk.crc);
      out->U64(chunk.count);
      out->U64(chunk.null_count);
      out->U64(chunk.distinct_count);
      EncodeZone(chunk.zone, meta.schema.at(i).type, out);
    }
  }
}

TableWriter::TableWriter(const std::string& path, Schema schema, const StorageConfig& config)
    : file_(path), config_(config) {
  meta_.schema = std::move(schema);
  meta_.rows = 0;
  buffers_.resize(meta_.schema.size());
  file_.Append(kMagic, sizeof(kMagic));
}

TableWriter::~TableWriter() {
  if (!finished_ && !broken_) {
    try {
      Finish();
    } catch (const std::exception&) {
    }
  }
}

void TableWriter::AppendInt(size_t column, int64_t value) {
  ColumnBuffer& buffer = buffers_[column];
  buffer.integers.push_back(value);
  buffer.valid.push_back(1);
}

void TableWriter::AppendReal(size_t column, double value) {
  ColumnBuffer& buffer = buffers_[column];
  buffer.reals.push_back(value);
  buffer.valid.push_back(1);
}

void TableWriter::AppendText(size_t column, const char* data, size_t size) {
  ColumnBuffer& buffer = buffers_[column];
  buffer.offsets.push_back(static_cast<uint32_t>(buffer.blob.size()));
  buffer.blob.insert(buffer.blob.end(), data, data + size);
  buffer.valid.push_back(1);
}

void TableWriter::AppendNull(size_t column) {
  ColumnBuffer& buffer = buffers_[column];
  const DataType type = meta_.schema.at(column).type;
  if (type == DataType::kString) {
    buffer.offsets.push_back(static_cast<uint32_t>(buffer.blob.size()));
  } else if (type == DataType::kDouble) {
    buffer.reals.push_back(0.0);
  } else {
    buffer.integers.push_back(0);
  }
  buffer.valid.push_back(0);
  ++buffer.nulls;
}

void TableWriter::EndRow() {
  for (size_t i = 0; i < buffers_.size(); ++i) {
    if (buffers_[i].valid.size() != staged_rows_ + 1) {
      broken_ = true;
      Fail("column " + meta_.schema.at(i).name + " is missing a value in this row");
    }
  }
  ++staged_rows_;
  if (staged_rows_ >= config_.row_group_rows) {
    FlushRowGroup();
  }
}

std::vector<uint8_t> TableWriter::EncodeFixed(size_t column, size_t rows,
                                              const std::vector<uint8_t>& validity,
                                              Encoding* encoding, ZoneMap* zone,
                                              uint64_t* distinct) {
  const ColumnSchema& schema = meta_.schema.at(column);
  ColumnBuffer& buffer = buffers_[column];
  ByteWriter payload;
  payload.buffer().insert(payload.buffer().end(), validity.begin(), validity.end());
  if (schema.type == DataType::kDouble) {
    *encoding = Encoding::kPlain;
    double smallest = std::numeric_limits<double>::max();
    double largest = std::numeric_limits<double>::lowest();
    bool any = false;
    for (size_t i = 0; i < rows; ++i) {
      if (buffer.valid[i] == 0) {
        continue;
      }
      any = true;
      smallest = std::min(smallest, buffer.reals[i]);
      largest = std::max(largest, buffer.reals[i]);
    }
    zone->valid = any;
    zone->min_real = any ? smallest : 0.0;
    zone->max_real = any ? largest : 0.0;
    *distinct = CountDistinctReal(buffer.reals, buffer.valid, rows);
    payload.Raw(buffer.reals.data(), rows * sizeof(double));
    return payload.buffer();
  }

  int64_t smallest = std::numeric_limits<int64_t>::max();
  int64_t largest = std::numeric_limits<int64_t>::min();
  bool any = false;
  for (size_t i = 0; i < rows; ++i) {
    if (buffer.valid[i] == 0) {
      continue;
    }
    any = true;
    smallest = std::min(smallest, buffer.integers[i]);
    largest = std::max(largest, buffer.integers[i]);
  }
  if (!any) {
    smallest = 0;
    largest = 0;
  }
  zone->valid = any;
  zone->min_integer = smallest;
  zone->max_integer = largest;
  *distinct = CountDistinctInteger(buffer.integers, buffer.valid, rows, smallest, largest);

  const uint64_t range = static_cast<uint64_t>(largest) - static_cast<uint64_t>(smallest);
  const uint32_t width = BitWidth(range);
  const size_t plain_width = TypeWidth(schema.type);
  if (config_.enable_bit_packing && width < plain_width * 8) {
    *encoding = Encoding::kBitPacked;
    payload.I64(smallest);
    payload.U8(static_cast<uint8_t>(width));
    std::vector<uint64_t> shifted(rows);
    for (size_t i = 0; i < rows; ++i) {
      shifted[i] = static_cast<uint64_t>(buffer.integers[i]) - static_cast<uint64_t>(smallest);
    }
    std::vector<uint8_t> packed;
    BitPack(shifted.data(), rows, width, &packed);
    payload.Raw(packed.data(), packed.size());
    return payload.buffer();
  }

  *encoding = Encoding::kPlain;
  if (plain_width == 4) {
    std::vector<int32_t> narrow(rows);
    for (size_t i = 0; i < rows; ++i) {
      narrow[i] = static_cast<int32_t>(buffer.integers[i]);
    }
    payload.Raw(narrow.data(), rows * sizeof(int32_t));
  } else {
    payload.Raw(buffer.integers.data(), rows * sizeof(int64_t));
  }
  return payload.buffer();
}

std::vector<uint8_t> TableWriter::EncodeText(size_t column, size_t rows,
                                             const std::vector<uint8_t>& validity,
                                             Encoding* encoding, ZoneMap* zone,
                                             uint64_t* distinct) {
  ColumnBuffer& buffer = buffers_[column];
  buffer.offsets.push_back(static_cast<uint32_t>(buffer.blob.size()));
  ByteWriter payload;
  payload.buffer().insert(payload.buffer().end(), validity.begin(), validity.end());

  std::string smallest;
  std::string largest;
  bool any = false;
  bool zone_usable = true;
  std::map<std::string, uint32_t> dictionary;
  std::vector<uint32_t> codes(rows, 0);
  bool use_dictionary = true;
  for (size_t i = 0; i < rows; ++i) {
    if (buffer.valid[i] == 0) {
      continue;
    }
    const uint32_t begin = buffer.offsets[i];
    const uint32_t end = buffer.offsets[i + 1];
    const std::string text(buffer.blob.data() + begin, buffer.blob.data() + end);
    if (text.size() > 16) {
      zone_usable = false;
    } else {
      if (!any || text < smallest) {
        smallest = text;
      }
      if (!any || text > largest) {
        largest = text;
      }
    }
    any = true;
    if (use_dictionary) {
      const auto found = dictionary.find(text);
      if (found != dictionary.end()) {
        codes[i] = found->second;
      } else {
        if (dictionary.size() >= config_.dictionary_max_cardinality) {
          use_dictionary = false;
        } else {
          const uint32_t code = static_cast<uint32_t>(dictionary.size());
          dictionary.emplace(text, code);
          codes[i] = code;
        }
      }
    }
  }
  zone->valid = any && zone_usable;
  zone->min_text = smallest;
  zone->max_text = largest;
  *distinct = use_dictionary ? dictionary.size() : rows;

  const double ratio =
      dictionary.empty() ? 0.0 : static_cast<double>(rows) / static_cast<double>(dictionary.size());
  if (use_dictionary && ratio >= config_.dictionary_min_repeat_ratio) {
    *encoding = Encoding::kDictionary;
    std::vector<const std::string*> ordered(dictionary.size(), nullptr);
    for (const auto& entry : dictionary) {
      ordered[entry.second] = &entry.first;
    }
    payload.U32(static_cast<uint32_t>(ordered.size()));
    uint32_t running = 0;
    for (const std::string* text : ordered) {
      payload.U32(running);
      running += static_cast<uint32_t>(text->size());
    }
    payload.U32(running);
    for (const std::string* text : ordered) {
      payload.Raw(text->data(), text->size());
    }
    const uint32_t width = BitWidth(ordered.empty() ? 0 : ordered.size() - 1);
    payload.U8(static_cast<uint8_t>(width));
    std::vector<uint64_t> wide(rows);
    for (size_t i = 0; i < rows; ++i) {
      wide[i] = codes[i];
    }
    std::vector<uint8_t> packed;
    BitPack(wide.data(), rows, width, &packed);
    payload.Raw(packed.data(), packed.size());
    return payload.buffer();
  }

  *encoding = Encoding::kPlainString;
  payload.Raw(buffer.offsets.data(), (rows + 1) * sizeof(uint32_t));
  payload.Raw(buffer.blob.data(), buffer.blob.size());
  return payload.buffer();
}

ChunkMeta TableWriter::WriteChunk(size_t column, size_t rows) {
  ColumnBuffer& buffer = buffers_[column];
  ChunkMeta chunk;
  chunk.count = rows;
  chunk.null_count = buffer.nulls;

  std::vector<uint8_t> validity;
  validity.push_back(buffer.nulls > 0 ? 1 : 0);
  if (buffer.nulls > 0) {
    std::vector<uint64_t> words((rows + 63) / 64, 0);
    for (size_t i = 0; i < rows; ++i) {
      if (buffer.valid[i] != 0) {
        words[i >> 6] |= 1ull << (i & 63);
      }
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(words.data());
    validity.insert(validity.end(), bytes, bytes + words.size() * sizeof(uint64_t));
  }

  std::vector<uint8_t> payload;
  if (meta_.schema.at(column).type == DataType::kString) {
    payload =
        EncodeText(column, rows, validity, &chunk.encoding, &chunk.zone, &chunk.distinct_count);
  } else {
    payload =
        EncodeFixed(column, rows, validity, &chunk.encoding, &chunk.zone, &chunk.distinct_count);
  }
  chunk.offset = file_.offset();
  chunk.size = payload.size();
  chunk.crc = config_.enable_checksums ? Crc32c(payload.data(), payload.size()) : 0;
  file_.Append(payload.data(), payload.size());
  return chunk;
}

void TableWriter::FlushRowGroup() {
  if (staged_rows_ == 0) {
    return;
  }
  RowGroupMeta group;
  group.rows = staged_rows_;
  for (size_t column = 0; column < buffers_.size(); ++column) {
    group.chunks.push_back(WriteChunk(column, staged_rows_));
  }
  meta_.row_groups.push_back(std::move(group));
  meta_.rows += staged_rows_;
  staged_rows_ = 0;
  for (ColumnBuffer& buffer : buffers_) {
    buffer.integers.clear();
    buffer.reals.clear();
    buffer.blob.clear();
    buffer.offsets.clear();
    buffer.valid.clear();
    buffer.nulls = 0;
  }
}

void TableWriter::Finish() {
  Check(!broken_, "the writer stopped on an incomplete row");
  if (finished_) {
    return;
  }
  FlushRowGroup();
  ByteWriter footer;
  EncodeMeta(meta_, &footer);
  const uint64_t footer_size = footer.buffer().size();
  const uint32_t footer_crc = Crc32c(footer.buffer().data(), footer.buffer().size());
  file_.Append(footer.buffer().data(), footer.buffer().size());
  file_.Append(&footer_size, sizeof(footer_size));
  file_.Append(&footer_crc, sizeof(footer_crc));
  file_.Append(kMagic, sizeof(kMagic));
  file_.Close();
  finished_ = true;
}

}
