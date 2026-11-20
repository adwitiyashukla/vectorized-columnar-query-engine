#pragma once

#include <cstdint>
#include <string>

namespace vcqe {

struct StorageConfig {
  uint32_t row_group_rows = 65536;
  uint32_t dictionary_max_cardinality = 65536;
  double dictionary_min_repeat_ratio = 2.0;
  bool enable_bit_packing = true;
  bool enable_checksums = true;
};

struct ExecutionConfig {
  std::string mode = "vectorized";
  uint32_t vector_size = 2048;
  uint32_t threads = 0;
  bool enable_simd = true;
  bool enable_zone_maps = true;
  bool enable_column_pruning = true;
  bool enable_predicate_pushdown = true;
  bool enable_join_reorder = true;
  double hash_table_load_factor = 0.6;
  uint32_t aggregate_initial_buckets = 1024;

  bool row_mode() const {
    return mode == "row";
  }
};

struct CsvConfig {
  char delimiter = '|';
  bool has_header = false;
  std::string null_string;
};

struct BenchConfig {
  uint32_t runs = 5;
  uint32_t warmup = 1;
};

struct Config {
  StorageConfig storage;
  ExecutionConfig execution;
  CsvConfig csv;
  BenchConfig bench;

  static Config Parse(const std::string& text);
  static Config Load(const std::string& path);
  void Validate() const;
  uint32_t ResolvedThreads() const;
};

}
