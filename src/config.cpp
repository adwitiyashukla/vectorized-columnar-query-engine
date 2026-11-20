#include "config.h"

#include <cstdlib>
#include <map>
#include <sstream>
#include <thread>

#include "error.h"
#include "util.h"

namespace vcqe {
namespace {

using Entries = std::map<std::string, std::string>;

Entries ParseEntries(const std::string& text) {
  Entries entries;
  std::istringstream stream(text);
  std::string line;
  std::string section;
  size_t line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    const size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    line = Trim(line);
    if (line.empty()) {
      continue;
    }
    const std::string where = " at config line " + std::to_string(line_number);
    if (line.front() == '[') {
      Check(line.back() == ']', "unterminated section header" + where);
      section = Trim(line.substr(1, line.size() - 2));
      Check(!section.empty(), "empty section header" + where);
      continue;
    }
    const size_t equals = line.find('=');
    Check(equals != std::string::npos, "expected key = value" + where);
    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    Check(!key.empty(), "empty key" + where);
    Check(!value.empty(), "empty value" + where);
    Check(!section.empty(), "key outside of a section" + where);
    const std::string full = section + "." + key;
    Check(entries.find(full) == entries.end(), "duplicate key " + full + where);
    entries[full] = value;
  }
  return entries;
}

bool ParseBool(const std::string& key, const std::string& text) {
  if (text == "true") {
    return true;
  }
  if (text == "false") {
    return false;
  }
  Fail("value for " + key + " must be true or false, found " + text);
}

int64_t ParseInt(const std::string& key, const std::string& text) {
  size_t consumed = 0;
  int64_t value = 0;
  try {
    value = std::stoll(text, &consumed);
  } catch (const std::exception&) {
    Fail("value for " + key + " must be an integer, found " + text);
  }
  Check(consumed == text.size(), "value for " + key + " must be an integer, found " + text);
  return value;
}

double ParseDouble(const std::string& key, const std::string& text) {
  size_t consumed = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &consumed);
  } catch (const std::exception&) {
    Fail("value for " + key + " must be a number, found " + text);
  }
  Check(consumed == text.size(), "value for " + key + " must be a number, found " + text);
  return value;
}

std::string ParseString(const std::string& key, const std::string& text) {
  Check(text.size() >= 2 && text.front() == '"' && text.back() == '"',
        "value for " + key + " must be a quoted string, found " + text);
  return text.substr(1, text.size() - 2);
}

class Reader {
public:
  explicit Reader(Entries entries) : entries_(std::move(entries)) {}

  void Bool(const std::string& key, bool* target) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    *target = ParseBool(key, it->second);
    entries_.erase(it);
  }

  void Unsigned(const std::string& key, uint32_t* target) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    const int64_t value = ParseInt(key, it->second);
    Check(value >= 0 && value <= 0x7FFFFFFF, "value for " + key + " is out of range");
    *target = static_cast<uint32_t>(value);
    entries_.erase(it);
  }

  void Number(const std::string& key, double* target) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    *target = ParseDouble(key, it->second);
    entries_.erase(it);
  }

  void Text(const std::string& key, std::string* target) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    *target = ParseString(key, it->second);
    entries_.erase(it);
  }

  void Character(const std::string& key, char* target) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    const std::string value = ParseString(key, it->second);
    Check(value.size() == 1, "value for " + key + " must be a single character");
    *target = value[0];
    entries_.erase(it);
  }

  void RejectRest() const {
    if (!entries_.empty()) {
      Fail("unknown config key: " + entries_.begin()->first);
    }
  }

private:
  Entries entries_;
};

}

Config Config::Parse(const std::string& text) {
  Config config;
  Reader reader(ParseEntries(text));
  reader.Unsigned("storage.row_group_rows", &config.storage.row_group_rows);
  reader.Unsigned("storage.dictionary_max_cardinality", &config.storage.dictionary_max_cardinality);
  reader.Number("storage.dictionary_min_repeat_ratio", &config.storage.dictionary_min_repeat_ratio);
  reader.Bool("storage.enable_bit_packing", &config.storage.enable_bit_packing);
  reader.Bool("storage.enable_checksums", &config.storage.enable_checksums);
  reader.Text("execution.mode", &config.execution.mode);
  reader.Unsigned("execution.vector_size", &config.execution.vector_size);
  reader.Unsigned("execution.threads", &config.execution.threads);
  reader.Bool("execution.enable_simd", &config.execution.enable_simd);
  reader.Bool("execution.enable_zone_maps", &config.execution.enable_zone_maps);
  reader.Bool("execution.enable_column_pruning", &config.execution.enable_column_pruning);
  reader.Bool("execution.enable_predicate_pushdown", &config.execution.enable_predicate_pushdown);
  reader.Bool("execution.enable_join_reorder", &config.execution.enable_join_reorder);
  reader.Number("execution.hash_table_load_factor", &config.execution.hash_table_load_factor);
  reader.Unsigned("execution.aggregate_initial_buckets",
                  &config.execution.aggregate_initial_buckets);
  reader.Character("csv.delimiter", &config.csv.delimiter);
  reader.Bool("csv.has_header", &config.csv.has_header);
  reader.Text("csv.null_string", &config.csv.null_string);
  reader.Unsigned("bench.runs", &config.bench.runs);
  reader.Unsigned("bench.warmup", &config.bench.warmup);
  reader.RejectRest();
  config.Validate();
  return config;
}

Config Config::Load(const std::string& path) {
  return Parse(ReadTextFile(path));
}

void Config::Validate() const {
  Check(storage.row_group_rows >= 1024 && storage.row_group_rows <= 4 * 1024 * 1024,
        "storage.row_group_rows must be between 1024 and 4194304");
  Check(storage.dictionary_max_cardinality >= 1 && storage.dictionary_max_cardinality <= 1u << 24,
        "storage.dictionary_max_cardinality must be between 1 and 16777216");
  Check(storage.dictionary_min_repeat_ratio >= 1.0 && storage.dictionary_min_repeat_ratio <= 1000.0,
        "storage.dictionary_min_repeat_ratio must be between 1.0 and 1000.0");
  Check(execution.mode == "vectorized" || execution.mode == "row",
        "execution.mode must be vectorized or row");
  Check(execution.vector_size >= 1 && execution.vector_size <= 65536,
        "execution.vector_size must be between 1 and 65536");
  Check(execution.threads <= 256, "execution.threads must be 256 or less");
  Check(execution.hash_table_load_factor >= 0.1 && execution.hash_table_load_factor <= 0.95,
        "execution.hash_table_load_factor must be between 0.1 and 0.95");
  Check(execution.aggregate_initial_buckets >= 16 &&
            execution.aggregate_initial_buckets <= 1u << 22,
        "execution.aggregate_initial_buckets must be between 16 and 4194304");
  Check(csv.delimiter != '\n' && csv.delimiter != '\r', "csv.delimiter must not be a line break");
  Check(bench.runs >= 1 && bench.runs <= 1000, "bench.runs must be between 1 and 1000");
  Check(bench.warmup <= 100, "bench.warmup must be 100 or less");
}

uint32_t Config::ResolvedThreads() const {
  if (execution.threads > 0) {
    return execution.threads;
  }
  const unsigned detected = std::thread::hardware_concurrency();
  return detected == 0 ? 1u : detected;
}

}
