#include "reader.h"

#include <algorithm>

#include "kernels.h"

namespace vcqe {
namespace {

ZoneMap DecodeZone(ByteReader* reader, DataType type) {
  ZoneMap zone;
  zone.valid = reader->U8() != 0;
  if (!zone.valid) {
    return zone;
  }
  if (type == DataType::kString) {
    zone.min_text = reader->Text();
    zone.max_text = reader->Text();
  } else if (type == DataType::kDouble) {
    zone.min_real = reader->F64();
    zone.max_real = reader->F64();
  } else {
    zone.min_integer = reader->I64();
    zone.max_integer = reader->I64();
  }
  return zone;
}

bool KnownEncoding(uint8_t value) {
  return value >= static_cast<uint8_t>(Encoding::kPlain) &&
         value <= static_cast<uint8_t>(Encoding::kPlainString);
}

bool KnownType(uint8_t value) {
  return value >= static_cast<uint8_t>(DataType::kInt32) &&
         value <= static_cast<uint8_t>(DataType::kString);
}

}

TableMeta DecodeMeta(const uint8_t* data, size_t size) {
  ByteReader reader(data, size);
  TableMeta meta;
  const uint32_t version = reader.U32();
  Check(version == kFormatVersion, "unsupported vcqe file version");
  meta.rows = reader.U64();
  const uint32_t columns = reader.U32();
  Check(columns > 0 && columns <= 4096, "bad column count in file metadata");
  for (uint32_t i = 0; i < columns; ++i) {
    ColumnSchema column;
    column.name = reader.Text();
    const uint8_t type = reader.U8();
    Check(KnownType(type), "bad column type in file metadata");
    column.type = static_cast<DataType>(type);
    column.scale = reader.U8();
    Check(column.scale <= 18, "bad decimal scale in file metadata");
    meta.schema.Add(std::move(column));
  }
  const uint32_t groups = reader.U32();
  uint64_t counted = 0;
  for (uint32_t g = 0; g < groups; ++g) {
    RowGroupMeta group;
    group.rows = reader.U64();
    Check(group.rows <= (1ull << 32), "bad row group size in file metadata");
    counted += group.rows;
    for (uint32_t c = 0; c < columns; ++c) {
      ChunkMeta chunk;
      const uint8_t encoding = reader.U8();
      Check(KnownEncoding(encoding), "bad chunk encoding in file metadata");
      chunk.encoding = static_cast<Encoding>(encoding);
      chunk.offset = reader.U64();
      chunk.size = reader.U64();
      chunk.crc = reader.U32();
      chunk.count = reader.U64();
      chunk.null_count = reader.U64();
      chunk.distinct_count = reader.U64();
      Check(chunk.distinct_count <= chunk.count,
            "chunk distinct count is larger than its row count");
      Check(chunk.count == group.rows, "chunk row count does not match its row group");
      Check(chunk.null_count <= chunk.count, "chunk null count is larger than its row count");
      chunk.zone = DecodeZone(&reader, meta.schema.at(c).type);
      group.chunks.push_back(std::move(chunk));
    }
    meta.row_groups.push_back(std::move(group));
  }
  Check(counted == meta.rows, "row group sizes do not add up to the table row count");
  return meta;
}

TableReader::TableReader(const std::string& path) : path_(path) {
  FileReader file(path);
  const uint64_t size = file.size();
  const uint64_t tail = sizeof(kMagic) + sizeof(uint64_t) + sizeof(uint32_t);
  Check(size >= sizeof(kMagic) + tail, "file is too small to be a vcqe file");
  char magic[sizeof(kMagic)];
  file.ReadAt(0, magic, sizeof(magic));
  Check(std::memcmp(magic, kMagic, sizeof(kMagic)) == 0, "missing vcqe header magic");
  file.ReadAt(size - sizeof(kMagic), magic, sizeof(magic));
  Check(std::memcmp(magic, kMagic, sizeof(kMagic)) == 0, "missing vcqe footer magic");
  uint64_t footer_size = 0;
  uint32_t footer_crc = 0;
  file.ReadAt(size - tail, &footer_size, sizeof(footer_size));
  file.ReadAt(size - tail + sizeof(uint64_t), &footer_crc, sizeof(footer_crc));
  const uint64_t footer_end = size - tail;
  Check(footer_size > 0 && footer_size <= footer_end - sizeof(kMagic),
        "footer size does not fit in the file");
  const std::vector<uint8_t> footer = file.ReadAt(footer_end - footer_size, footer_size);
  Check(Crc32c(footer.data(), footer.size()) == footer_crc, "checksum mismatch in file metadata");
  meta_ = DecodeMeta(footer.data(), footer.size());
  const uint64_t data_end = footer_end - footer_size;
  for (const RowGroupMeta& group : meta_.row_groups) {
    for (const ChunkMeta& chunk : group.chunks) {
      Check(chunk.offset >= sizeof(kMagic) && chunk.size <= data_end &&
                chunk.offset + chunk.size <= data_end && chunk.offset + chunk.size >= chunk.offset,
            "chunk range points outside the file");
    }
  }
}

ChunkCursor::ChunkCursor(const TableReader& table) : table_(table), file_(table.path()) {}

void ChunkCursor::Read(size_t group, size_t column, ColumnChunk* out) {
  const ChunkMeta& chunk = table_.Chunk(group, column);
  const ColumnSchema& schema = table_.schema().at(column);
  scratch_.resize(static_cast<size_t>(chunk.size));
  file_.ReadAt(chunk.offset, scratch_.data(), scratch_.size());
  if (chunk.crc != 0) {
    Check(Crc32c(scratch_.data(), scratch_.size()) == chunk.crc,
          "checksum mismatch in column " + schema.name);
  }

  out->type = schema.type;
  out->scale = schema.scale;
  out->count = static_cast<size_t>(chunk.count);
  out->fixed.clear();
  out->strings.clear();
  out->blob.clear();
  out->validity.clear();
  out->all_valid = true;

  ByteReader reader(scratch_.data(), scratch_.size());
  const bool has_nulls = reader.U8() != 0;
  if (has_nulls) {
    const size_t words = (out->count + 63) / 64;
    out->validity.resize(words);
    reader.Raw(out->validity.data(), words * sizeof(uint64_t));
    out->all_valid = false;
  }

  const size_t count = out->count;
  if (schema.type == DataType::kString) {
    if (chunk.encoding == Encoding::kDictionary) {
      const uint32_t entries = reader.U32();
      Check(entries <= count + 1, "dictionary is larger than the chunk");
      std::vector<uint32_t> offsets(entries + 1);
      for (uint32_t i = 0; i <= entries; ++i) {
        offsets[i] = reader.U32();
      }
      for (uint32_t i = 0; i < entries; ++i) {
        Check(offsets[i] <= offsets[i + 1], "dictionary offsets are not sorted");
      }
      const uint32_t blob_size = entries == 0 ? 0 : offsets[entries];
      const uint8_t* blob = reader.Take(blob_size);
      out->blob.assign(blob, blob + blob_size);
      const uint32_t width = reader.U8();
      Check(width <= 32, "bad dictionary code width");
      std::vector<uint64_t> codes(count);
      const size_t packed_size = (count * width + 7) / 8;
      const uint8_t* packed = reader.Take(packed_size);
      BitUnpack(packed, packed_size, count, width, codes.data());
      out->strings.resize(count);
      for (size_t i = 0; i < count; ++i) {
        const uint64_t code = codes[i];
        Check(entries == 0 ? code == 0 : code < entries, "dictionary code is out of range");
        if (entries == 0) {
          out->strings[i] = StringRef{out->blob.data(), 0};
        } else {
          out->strings[i] =
              StringRef{out->blob.data() + offsets[code], offsets[code + 1] - offsets[code]};
        }
      }
    } else {
      Check(chunk.encoding == Encoding::kPlainString, "unexpected encoding for a string column");
      std::vector<uint32_t> offsets(count + 1);
      for (size_t i = 0; i <= count; ++i) {
        offsets[i] = reader.U32();
      }
      for (size_t i = 0; i < count; ++i) {
        Check(offsets[i] <= offsets[i + 1], "string offsets are not sorted");
      }
      const uint32_t blob_size = count == 0 ? 0 : offsets[count];
      const uint8_t* blob = reader.Take(blob_size);
      out->blob.assign(blob, blob + blob_size);
      out->strings.resize(count);
      for (size_t i = 0; i < count; ++i) {
        out->strings[i] = StringRef{out->blob.data() + offsets[i], offsets[i + 1] - offsets[i]};
      }
    }
    return;
  }

  const size_t width = TypeWidth(schema.type);
  out->fixed.resize(count * width);
  if (chunk.encoding == Encoding::kPlain) {
    const uint8_t* values = reader.Take(count * width);
    std::memcpy(out->fixed.data(), values, count * width);
    return;
  }
  Check(chunk.encoding == Encoding::kBitPacked, "unexpected encoding for a fixed width column");
  const int64_t base = reader.I64();
  const uint32_t bits = reader.U8();
  Check(bits <= 64, "bad bit width");
  const size_t packed_size = (count * bits + 7) / 8;
  const uint8_t* packed = reader.Take(packed_size);
  std::vector<uint64_t> raw(count);
  BitUnpack(packed, packed_size, count, bits, raw.data());
  if (width == 4) {
    int32_t* target = reinterpret_cast<int32_t*>(out->fixed.data());
    for (size_t i = 0; i < count; ++i) {
      target[i] = static_cast<int32_t>(static_cast<uint64_t>(base) + raw[i]);
    }
  } else {
    int64_t* target = reinterpret_cast<int64_t*>(out->fixed.data());
    for (size_t i = 0; i < count; ++i) {
      target[i] = static_cast<int64_t>(static_cast<uint64_t>(base) + raw[i]);
    }
  }
}

}
