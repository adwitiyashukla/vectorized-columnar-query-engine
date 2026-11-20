#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "csv.h"
#include "engine.h"
#include "error.h"
#include "gen.h"
#include "kernels.h"
#include "util.h"

namespace vcqe {
namespace {

struct Options {
  std::map<std::string, std::string> values;

  bool Has(const std::string& key) const {
    return values.count(key) > 0;
  }
  std::string Get(const std::string& key, const std::string& fallback) const {
    const auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
  }
  int64_t Number(const std::string& key, int64_t fallback) const {
    const auto found = values.find(key);
    return found == values.end() ? fallback : std::stoll(found->second);
  }
  double Real(const std::string& key, double fallback) const {
    const auto found = values.find(key);
    return found == values.end() ? fallback : std::stod(found->second);
  }
};

Options ParseOptions(int argc, char** argv, int start) {
  Options options;
  for (int i = start; i < argc; ++i) {
    std::string argument = argv[i];
    Check(StartsWith(argument, "--"), "unexpected argument: " + argument);
    argument = argument.substr(2);
    const size_t equals = argument.find('=');
    if (equals != std::string::npos) {
      options.values[argument.substr(0, equals)] = argument.substr(equals + 1);
      continue;
    }
    if (i + 1 < argc && !StartsWith(argv[i + 1], "--")) {
      options.values[argument] = argv[++i];
    } else {
      options.values[argument] = "true";
    }
  }
  return options;
}

Config LoadConfig(const Options& options) {
  const std::string path = options.Get("config", "config.toml");
  if (std::filesystem::exists(path)) {
    return Config::Load(path);
  }
  Config config;
  config.Validate();
  return config;
}

void PrintRows(const QueryResult& result, int64_t limit) {
  if (limit == 0) {
    return;
  }
  std::string header;
  for (size_t i = 0; i < result.schema.size(); ++i) {
    if (i > 0) {
      header += "|";
    }
    header += result.schema.at(i).name;
  }
  std::cout << header << "\n";
  for (size_t i = 0; i < result.rows.size(); ++i) {
    if (limit > 0 && static_cast<int64_t>(i) >= limit) {
      std::cout << "... " << result.rows.size() - static_cast<size_t>(limit) << " more rows\n";
      break;
    }
    std::string line;
    for (size_t c = 0; c < result.rows[i].size(); ++c) {
      if (c > 0) {
        line += "|";
      }
      line += FormatValue(result.rows[i][c]);
    }
    std::cout << line << "\n";
  }
}

std::string ReadQuery(const Options& options) {
  if (options.Has("sql")) {
    return options.Get("sql", "");
  }
  Check(options.Has("file"), "pass either --sql or --file");
  return ReadTextFile(options.Get("file", ""));
}

int RunGenerate(const Options& options) {
  const std::string directory = options.Get("dir", "data");
  std::filesystem::create_directories(directory);
  const GenerateReport report = GenerateTpch(directory, options.Real("scale", 1.0));
  for (const auto& entry : report.tables) {
    std::cout << entry.first << " " << entry.second << " rows\n";
  }
  std::printf("generated in %.2f seconds\n", report.seconds);
  return 0;
}

int RunLoad(const Options& options) {
  const std::string directory = options.Get("dir", "data");
  std::filesystem::create_directories(directory);
  Config config = LoadConfig(options);
  Engine engine(config, directory);
  std::vector<std::string> tables;
  if (options.Has("all")) {
    tables = TpchTables();
  } else {
    Check(options.Has("table"), "pass --table or --all");
    tables.push_back(options.Get("table", ""));
  }
  uint64_t total_rows = 0;
  uint64_t total_source = 0;
  uint64_t total_target = 0;
  double seconds = 0.0;
  for (const std::string& table : tables) {
    const std::string source =
        options.Get("source", (std::filesystem::path(directory) / (table + ".tbl")).string());
    const Schema schema =
        options.Has("schema") ? ParseSchemaText(options.Get("schema", "")) : TpchSchema(table);
    const LoadReport report = engine.Load(table, source, schema);
    std::printf("%-10s %9llu rows  %7.2f MB csv  %7.2f MB vcqe  %5.2fx  %.2fs\n", table.c_str(),
                static_cast<unsigned long long>(report.rows),
                static_cast<double>(report.source_bytes) / 1048576.0,
                static_cast<double>(report.target_bytes) / 1048576.0,
                report.target_bytes == 0 ? 0.0
                                         : static_cast<double>(report.source_bytes) /
                                               static_cast<double>(report.target_bytes),
                report.seconds);
    total_rows += report.rows;
    total_source += report.source_bytes;
    total_target += report.target_bytes;
    seconds += report.seconds;
  }
  if (tables.size() > 1) {
    std::printf(
        "total      %9llu rows  %7.2f MB csv  %7.2f MB vcqe  %5.2fx  %.2fs\n",
        static_cast<unsigned long long>(total_rows), static_cast<double>(total_source) / 1048576.0,
        static_cast<double>(total_target) / 1048576.0,
        total_target == 0 ? 0.0
                          : static_cast<double>(total_source) / static_cast<double>(total_target),
        seconds);
  }
  return 0;
}

int RunTables(const Options& options) {
  Catalog catalog(options.Get("dir", "data"));
  for (const std::string& table : catalog.Tables()) {
    const TableReader& reader = catalog.Open(table);
    std::printf("%-10s %9llu rows  %4llu row groups  %2llu columns\n", table.c_str(),
                static_cast<unsigned long long>(reader.rows()),
                static_cast<unsigned long long>(reader.row_group_count()),
                static_cast<unsigned long long>(reader.schema().size()));
  }
  return 0;
}

int RunQuery(const Options& options) {
  Config config = LoadConfig(options);
  if (options.Has("mode")) {
    config.execution.mode = options.Get("mode", "vectorized");
  }
  if (options.Has("threads")) {
    config.execution.threads = static_cast<uint32_t>(options.Number("threads", 0));
  }
  if (options.Has("vector-size")) {
    config.execution.vector_size = static_cast<uint32_t>(options.Number("vector-size", 2048));
  }
  if (options.Has("no-zone-maps")) {
    config.execution.enable_zone_maps = false;
  }
  if (options.Has("no-simd")) {
    config.execution.enable_simd = false;
  }
  config.Validate();
  Engine engine(config, options.Get("dir", "data"));
  const std::string sql = ReadQuery(options);
  if (options.Has("explain")) {
    std::cout << engine.Explain(sql);
    return 0;
  }
  const QueryRun run = engine.Query(sql);
  PrintRows(run.result, options.Number("print", 20));
  if (options.Has("quiet")) {
    return 0;
  }
  std::printf("%zu rows in %.3f seconds  [scanned %llu rows, skipped %llu of %llu row groups]\n",
              run.result.rows.size(), run.seconds,
              static_cast<unsigned long long>(run.result.stats.rows_scanned),
              static_cast<unsigned long long>(run.result.stats.row_groups_skipped),
              static_cast<unsigned long long>(run.result.stats.row_groups_total));
  return 0;
}

double BestOf(Engine* engine, const std::string& sql, uint32_t warmup, uint32_t runs,
              size_t* rows) {
  double best = 0.0;
  for (uint32_t i = 0; i < warmup; ++i) {
    engine->Query(sql);
  }
  for (uint32_t i = 0; i < runs; ++i) {
    const QueryRun run = engine->Query(sql);
    *rows = run.result.rows.size();
    if (i == 0 || run.seconds < best) {
      best = run.seconds;
    }
  }
  return best;
}

int RunBench(const Options& options) {
  Config config = LoadConfig(options);
  config.Validate();
  const std::string directory = options.Get("dir", "data");
  const std::string queries = options.Get("queries", "queries");
  const uint32_t runs = static_cast<uint32_t>(options.Number("runs", config.bench.runs));
  const uint32_t warmup = static_cast<uint32_t>(options.Number("warmup", config.bench.warmup));
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(queries)) {
    if (entry.path().extension() == ".sql") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  Check(!files.empty(), "no .sql files in " + queries);

  Engine engine(config, directory);
  std::printf("%-6s %10s %10s %10s %10s %8s\n", "query", "vector", "1 thread", "row", "speedup",
              "rows");
  for (const std::filesystem::path& file : files) {
    const std::string sql = ReadTextFile(file.string());
    size_t rows = 0;
    engine.set_mode("vectorized");
    engine.set_threads(config.execution.threads);
    const double parallel = BestOf(&engine, sql, warmup, runs, &rows);
    engine.set_threads(1);
    const double single = BestOf(&engine, sql, warmup, runs, &rows);
    engine.set_mode("row");
    const double row = BestOf(
        &engine, sql, 0,
        options.Has("row-runs") ? static_cast<uint32_t>(options.Number("row-runs", 1)) : 1, &rows);
    std::printf("%-6s %10.3f %10.3f %10.3f %9.1fx %8zu\n", file.stem().string().c_str(), parallel,
                single, row, single > 0 ? row / single : 0.0, rows);
  }

  if (options.Has("sweep")) {
    const std::string sql = ReadTextFile(files.front().string());
    std::printf("\nvector size sweep on %s (one thread)\n", files.front().stem().string().c_str());
    for (uint32_t size : {1u, 8u, 64u, 512u, 2048u, 8192u}) {
      Config sweep = config;
      sweep.execution.vector_size = size;
      sweep.execution.threads = 1;
      sweep.Validate();
      Engine local(sweep, directory);
      size_t rows = 0;
      const double seconds = BestOf(&local, sql, warmup, runs, &rows);
      std::printf("%6u %10.3f\n", size, seconds);
    }
  }
  return 0;
}

int Usage() {
  std::cout << "usage: vcqe <command> [options]\n"
            << "  gen     --dir data --scale 1\n"
            << "  load    --dir data --all\n"
            << "  tables  --dir data\n"
            << "  query   --dir data --file queries/q1.sql [--mode row] [--threads 4] [--explain]\n"
            << "  bench   --dir data --queries queries [--runs 5] [--sweep]\n";
  return 1;
}

int Main(int argc, char** argv) {
  if (argc < 2) {
    return Usage();
  }
  const std::string command = argv[1];
  const Options options = ParseOptions(argc, argv, 2);
  if (command == "gen") {
    return RunGenerate(options);
  }
  if (command == "load") {
    return RunLoad(options);
  }
  if (command == "tables") {
    return RunTables(options);
  }
  if (command == "query") {
    return RunQuery(options);
  }
  if (command == "bench") {
    return RunBench(options);
  }
  return Usage();
}

}
}

int main(int argc, char** argv) {
  try {
    return vcqe::Main(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
