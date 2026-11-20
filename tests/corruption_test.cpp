#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "config.h"
#include "harness.h"
#include "reader.h"
#include "writer.h"

using namespace vcqe;

namespace {

std::string GoodFile() {
  const std::string path =
      (std::filesystem::temp_directory_path() / "vcqe_corrupt_source.vcq").string();
  Config config;
  config.storage.row_group_rows = 1024;
  Schema schema;
  schema.Add(ColumnSchema{"id", DataType::kInt64, 0});
  schema.Add(ColumnSchema{"label", DataType::kString, 0});
  TableWriter writer(path, schema, config.storage);
  for (size_t i = 0; i < 3000; ++i) {
    writer.AppendInt(0, static_cast<int64_t>(i));
    const std::string label = "label" + std::to_string(i % 7);
    writer.AppendText(1, label.data(), label.size());
    writer.EndRow();
  }
  writer.Finish();
  return path;
}

std::vector<uint8_t> ReadAll(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
}

std::string WriteCopy(const std::vector<uint8_t>& bytes, const std::string& name) {
  const std::string path =
      (std::filesystem::temp_directory_path() / ("vcqe_corrupt_" + name + ".vcq")).string();
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return path;
}

void ExpectRejected(const std::string& path) {
  bool threw = false;
  try {
    TableReader reader(path);
    ChunkCursor cursor(reader);
    ColumnChunk chunk;
    for (size_t group = 0; group < reader.row_group_count(); ++group) {
      for (size_t column = 0; column < reader.schema().size(); ++column) {
        cursor.Read(group, column, &chunk);
        for (size_t i = 0; i < chunk.count; ++i) {
          if (chunk.type == DataType::kString) {
            volatile size_t size = chunk.strings[i].ToString().size();
            (void)size;
          }
        }
      }
    }
  } catch (const std::exception&) {
    threw = true;
  }
  if (!threw) {
    vcqe::Report(__FILE__, __LINE__, "corrupt file " + path + " was accepted");
  }
}

}

TEST(MissingFileIsRejected) {
  EXPECT_THROWS(TableReader("/tmp/vcqe_does_not_exist.vcq"));
}

TEST(TruncatedFileIsRejected) {
  const std::vector<uint8_t> bytes = ReadAll(GoodFile());
  for (size_t keep : {0u, 8u, 100u, 1000u}) {
    if (keep >= bytes.size()) {
      continue;
    }
    std::vector<uint8_t> copy(bytes.begin(), bytes.begin() + keep);
    ExpectRejected(WriteCopy(copy, "trunc" + std::to_string(keep)));
  }
  std::vector<uint8_t> almost(bytes.begin(), bytes.end() - 32);
  ExpectRejected(WriteCopy(almost, "trunc_tail"));
}

TEST(BadMagicIsRejected) {
  std::vector<uint8_t> bytes = ReadAll(GoodFile());
  bytes[2] = 'X';
  ExpectRejected(WriteCopy(bytes, "magic"));
  bytes = ReadAll(GoodFile());
  bytes[bytes.size() - 3] = 'X';
  ExpectRejected(WriteCopy(bytes, "footer_magic"));
}

TEST(BadFooterSizeIsRejected) {
  std::vector<uint8_t> bytes = ReadAll(GoodFile());
  const size_t position = bytes.size() - 20;
  uint64_t huge = 1ull << 40;
  std::memcpy(bytes.data() + position, &huge, sizeof(huge));
  ExpectRejected(WriteCopy(bytes, "footer_size"));
  bytes = ReadAll(GoodFile());
  uint64_t zero = 0;
  std::memcpy(bytes.data() + position, &zero, sizeof(zero));
  ExpectRejected(WriteCopy(bytes, "footer_zero"));
}

TEST(CorruptChunkBytesAreCaught) {
  const std::vector<uint8_t> original = ReadAll(GoodFile());
  for (size_t offset : {20u, 500u, 5000u}) {
    if (offset >= original.size()) {
      continue;
    }
    std::vector<uint8_t> bytes = original;
    bytes[offset] = static_cast<uint8_t>(bytes[offset] + 1);
    ExpectRejected(WriteCopy(bytes, "chunk" + std::to_string(offset)));
  }
}

TEST(CorruptMetadataIsCaught) {
  const std::vector<uint8_t> original = ReadAll(GoodFile());
  for (size_t back = 20; back < 140; back += 7) {
    if (back >= original.size()) {
      break;
    }
    std::vector<uint8_t> bytes = original;
    const size_t position = bytes.size() - back;
    bytes[position] = static_cast<uint8_t>(bytes[position] ^ 0x5A);
    ExpectRejected(WriteCopy(bytes, "meta" + std::to_string(back)));
  }
}

TEST_MAIN()
