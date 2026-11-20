#include <filesystem>
#include <random>

#include "config.h"
#include "harness.h"
#include "reader.h"
#include "writer.h"

using namespace vcqe;

namespace {

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("vcqe_" + name + ".vcq")).string();
}

Schema MixedSchema() {
  Schema schema;
  schema.Add(ColumnSchema{"id", DataType::kInt64, 0});
  schema.Add(ColumnSchema{"small", DataType::kInt32, 0});
  schema.Add(ColumnSchema{"price", DataType::kDecimal, 2});
  schema.Add(ColumnSchema{"ratio", DataType::kDouble, 0});
  schema.Add(ColumnSchema{"day", DataType::kDate, 0});
  schema.Add(ColumnSchema{"label", DataType::kString, 0});
  return schema;
}

}

TEST(RoundTripEveryType) {
  const std::string path = TempPath("types");
  Config config;
  config.storage.row_group_rows = 1024;
  const size_t rows = 5000;
  {
    TableWriter writer(path, MixedSchema(), config.storage);
    for (size_t i = 0; i < rows; ++i) {
      writer.AppendInt(0, static_cast<int64_t>(i) * 1000);
      writer.AppendInt(1, static_cast<int64_t>(i % 97));
      writer.AppendInt(2, static_cast<int64_t>(i) * 7);
      writer.AppendReal(3, static_cast<double>(i) / 3.0);
      writer.AppendInt(4, ParseDate("1995-01-01") + static_cast<int64_t>(i % 365));
      const std::string label = "label" + std::to_string(i % 13);
      writer.AppendText(5, label.data(), label.size());
      writer.EndRow();
    }
    writer.Finish();
  }
  TableReader reader(path);
  EXPECT_EQ(reader.rows(), rows);
  EXPECT_EQ(reader.row_group_count(), 5u);
  EXPECT_EQ(reader.schema().size(), 6u);
  ChunkCursor cursor(reader);
  ColumnChunk chunk;
  size_t seen = 0;
  for (size_t group = 0; group < reader.row_group_count(); ++group) {
    cursor.Read(group, 0, &chunk);
    for (size_t i = 0; i < chunk.count; ++i) {
      EXPECT_EQ(chunk.i64()[i], static_cast<int64_t>(seen + i) * 1000);
    }
    cursor.Read(group, 2, &chunk);
    for (size_t i = 0; i < chunk.count; ++i) {
      EXPECT_EQ(chunk.i64()[i], static_cast<int64_t>(seen + i) * 7);
    }
    cursor.Read(group, 3, &chunk);
    for (size_t i = 0; i < chunk.count; ++i) {
      EXPECT(chunk.f64()[i] == static_cast<double>(seen + i) / 3.0);
    }
    cursor.Read(group, 5, &chunk);
    for (size_t i = 0; i < chunk.count; ++i) {
      EXPECT_EQ(chunk.strings[i].ToString(), "label" + std::to_string((seen + i) % 13));
    }
    seen += chunk.count;
  }
  EXPECT_EQ(seen, rows);
}

TEST(ZoneMapsAndDistinctCounts) {
  const std::string path = TempPath("zones");
  Config config;
  config.storage.row_group_rows = 1024;
  {
    TableWriter writer(path, MixedSchema(), config.storage);
    for (size_t i = 0; i < 2048; ++i) {
      writer.AppendInt(0, static_cast<int64_t>(i));
      writer.AppendInt(1, static_cast<int64_t>(i % 4));
      writer.AppendInt(2, 100);
      writer.AppendReal(3, 1.5);
      writer.AppendInt(4, ParseDate("2000-06-15"));
      const std::string label = "row";
      writer.AppendText(5, label.data(), label.size());
      writer.EndRow();
    }
    writer.Finish();
  }
  TableReader reader(path);
  EXPECT_EQ(reader.Chunk(0, 0).zone.min_integer, 0);
  EXPECT_EQ(reader.Chunk(0, 0).zone.max_integer, 1023);
  EXPECT_EQ(reader.Chunk(1, 0).zone.min_integer, 1024);
  EXPECT_EQ(reader.Chunk(0, 0).distinct_count, 1024u);
  EXPECT_EQ(reader.Chunk(0, 1).distinct_count, 4u);
  EXPECT_EQ(reader.Chunk(0, 5).distinct_count, 1u);
  EXPECT_EQ(reader.Chunk(0, 5).zone.min_text, std::string("row"));
  EXPECT_EQ(reader.Chunk(0, 2).encoding, Encoding::kBitPacked);
  EXPECT_EQ(reader.Chunk(0, 5).encoding, Encoding::kDictionary);
}

TEST(NullsSurviveTheRoundTrip) {
  const std::string path = TempPath("nulls");
  Config config;
  config.storage.row_group_rows = 1024;
  {
    TableWriter writer(path, MixedSchema(), config.storage);
    for (size_t i = 0; i < 100; ++i) {
      if (i % 3 == 0) {
        writer.AppendNull(0);
      } else {
        writer.AppendInt(0, static_cast<int64_t>(i));
      }
      writer.AppendInt(1, 0);
      writer.AppendInt(2, 0);
      writer.AppendReal(3, 0.0);
      writer.AppendInt(4, 0);
      if (i % 5 == 0) {
        writer.AppendNull(5);
      } else {
        writer.AppendText(5, "x", 1);
      }
      writer.EndRow();
    }
    writer.Finish();
  }
  TableReader reader(path);
  EXPECT_EQ(reader.Chunk(0, 0).null_count, 34u);
  ChunkCursor cursor(reader);
  ColumnChunk chunk;
  cursor.Read(0, 0, &chunk);
  for (size_t i = 0; i < chunk.count; ++i) {
    EXPECT_EQ(chunk.IsValid(i), i % 3 != 0);
  }
  cursor.Read(0, 5, &chunk);
  for (size_t i = 0; i < chunk.count; ++i) {
    EXPECT_EQ(chunk.IsValid(i), i % 5 != 0);
  }
}

TEST(PlainEncodingForWideValues) {
  const std::string path = TempPath("wide");
  Config config;
  config.storage.row_group_rows = 1024;
  Schema schema;
  schema.Add(ColumnSchema{"id", DataType::kInt64, 0});
  schema.Add(ColumnSchema{"text", DataType::kString, 0});
  {
    TableWriter writer(path, schema, config.storage);
    std::mt19937_64 random(3);
    for (size_t i = 0; i < 2000; ++i) {
      writer.AppendInt(0, static_cast<int64_t>(random()));
      const std::string text = "unique" + std::to_string(i);
      writer.AppendText(1, text.data(), text.size());
      writer.EndRow();
    }
    writer.Finish();
  }
  TableReader reader(path);
  EXPECT_EQ(reader.Chunk(0, 0).encoding, Encoding::kPlain);
  EXPECT_EQ(reader.Chunk(0, 1).encoding, Encoding::kPlainString);
  ChunkCursor cursor(reader);
  ColumnChunk chunk;
  cursor.Read(0, 1, &chunk);
  EXPECT_EQ(chunk.strings[7].ToString(), std::string("unique7"));
}

TEST(EmptyTableIsReadable) {
  const std::string path = TempPath("empty");
  Config config;
  {
    TableWriter writer(path, MixedSchema(), config.storage);
    writer.Finish();
  }
  TableReader reader(path);
  EXPECT_EQ(reader.rows(), 0u);
  EXPECT_EQ(reader.row_group_count(), 0u);
}

TEST(RowsNeedEveryColumn) {
  const std::string path = TempPath("partial");
  Config config;
  TableWriter writer(path, MixedSchema(), config.storage);
  writer.AppendInt(0, 1);
  EXPECT_THROWS(writer.EndRow());
}

TEST_MAIN()
