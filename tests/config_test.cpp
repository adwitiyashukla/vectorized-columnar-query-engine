#include "config.h"

#include "error.h"
#include "harness.h"

using namespace vcqe;

TEST(DefaultsAreValid) {
  Config config;
  config.Validate();
  EXPECT_EQ(config.execution.vector_size, 2048u);
  EXPECT_EQ(config.storage.row_group_rows, 65536u);
  EXPECT(config.ResolvedThreads() >= 1);
}

TEST(ParsesEveryKey) {
  const std::string text =
      "[storage]\nrow_group_rows = 4096\ndictionary_max_cardinality = 100\n"
      "dictionary_min_repeat_ratio = 3.5\nenable_bit_packing = false\nenable_checksums = false\n"
      "[execution]\nmode = \"row\"\nvector_size = 512\nthreads = 3\nenable_simd = false\n"
      "enable_zone_maps = false\nenable_column_pruning = false\nenable_predicate_pushdown = false\n"
      "enable_join_reorder = false\nhash_table_load_factor = 0.5\naggregate_initial_buckets = 64\n"
      "[csv]\ndelimiter = \",\"\nhas_header = true\nnull_string = \"NA\"\n"
      "[bench]\nruns = 2\nwarmup = 0\n";
  const Config config = Config::Parse(text);
  EXPECT_EQ(config.storage.row_group_rows, 4096u);
  EXPECT_EQ(config.storage.dictionary_max_cardinality, 100u);
  EXPECT(config.storage.dictionary_min_repeat_ratio == 3.5);
  EXPECT(!config.storage.enable_bit_packing);
  EXPECT(!config.storage.enable_checksums);
  EXPECT(config.execution.row_mode());
  EXPECT_EQ(config.execution.vector_size, 512u);
  EXPECT_EQ(config.ResolvedThreads(), 3u);
  EXPECT(!config.execution.enable_simd);
  EXPECT(!config.execution.enable_join_reorder);
  EXPECT_EQ(config.csv.delimiter, ',');
  EXPECT(config.csv.has_header);
  EXPECT_EQ(config.csv.null_string, std::string("NA"));
  EXPECT_EQ(config.bench.runs, 2u);
}

TEST(CommentsAndBlankLines) {
  const Config config = Config::Parse("# comment\n\n[execution]\nvector_size = 64 # trailing\n");
  EXPECT_EQ(config.execution.vector_size, 64u);
}

TEST(RejectsBadInput) {
  EXPECT_THROWS(Config::Parse("[execution]\nunknown_key = 1\n"));
  EXPECT_THROWS(Config::Parse("vector_size = 64\n"));
  EXPECT_THROWS(Config::Parse("[execution]\nvector_size = abc\n"));
  EXPECT_THROWS(Config::Parse("[execution]\nenable_simd = 1\n"));
  EXPECT_THROWS(Config::Parse("[execution]\nmode = vectorized\n"));
  EXPECT_THROWS(Config::Parse("[execution]\nmode = \"sideways\"\n"));
  EXPECT_THROWS(Config::Parse("[execution]\nvector_size = 0\n"));
  EXPECT_THROWS(Config::Parse("[execution]\nvector_size = 100000\n"));
  EXPECT_THROWS(Config::Parse("[storage]\nrow_group_rows = 8\n"));
  EXPECT_THROWS(Config::Parse("[execution]\nhash_table_load_factor = 0.99\n"));
  EXPECT_THROWS(Config::Parse("[csv]\ndelimiter = \"||\"\n"));
  EXPECT_THROWS(Config::Parse("[execution]\nvector_size = 64\nvector_size = 128\n"));
  EXPECT_THROWS(Config::Parse("[execution\nvector_size = 64\n"));
}

TEST_MAIN()
