#include "gen.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "error.h"
#include "util.h"

namespace vcqe {
namespace {

class Random {
public:
  explicit Random(uint64_t seed) : state_(seed == 0 ? 1 : seed) {}
  uint64_t Next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 2685821657736338717ull;
  }
  int64_t Between(int64_t low, int64_t high) {
    return low + static_cast<int64_t>(Next() % static_cast<uint64_t>(high - low + 1));
  }

private:
  uint64_t state_;
};

const char* kRegions[] = {"AFRICA", "AMERICA", "ASIA", "EUROPE", "MIDDLE EAST"};

struct NationRow {
  const char* name;
  int region;
};

const NationRow kNations[] = {
    {"ALGERIA", 0},      {"ARGENTINA", 1}, {"BRAZIL", 1}, {"CANADA", 1},
    {"EGYPT", 4},        {"ETHIOPIA", 0},  {"FRANCE", 3}, {"GERMANY", 3},
    {"INDIA", 2},        {"INDONESIA", 2}, {"IRAN", 4},   {"IRAQ", 4},
    {"JAPAN", 2},        {"JORDAN", 4},    {"KENYA", 0},  {"MOROCCO", 0},
    {"MOZAMBIQUE", 0},   {"PERU", 1},      {"CHINA", 2},  {"ROMANIA", 3},
    {"SAUDI ARABIA", 4}, {"VIETNAM", 2},   {"RUSSIA", 3}, {"UNITED KINGDOM", 3},
    {"UNITED STATES", 1}};

const char* kSegments[] = {"AUTOMOBILE", "BUILDING", "FURNITURE", "MACHINERY", "HOUSEHOLD"};
const char* kPriorities[] = {"1-URGENT", "2-HIGH", "3-MEDIUM", "4-NOT SPECIFIED", "5-LOW"};
const char* kInstructions[] = {"DELIVER IN PERSON", "COLLECT COD", "NONE", "TAKE BACK RETURN"};
const char* kModes[] = {"REG AIR", "AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB"};
const char* kContainers[] = {"SM CASE", "SM BOX",   "SM PACK", "SM PKG", "MED BAG", "MED BOX",
                             "MED PKG", "MED CASE", "LG CASE", "LG BOX", "LG PACK", "LG PKG"};
const char* kMaterials[] = {"STANDARD", "SMALL", "MEDIUM", "LARGE", "ECONOMY", "PROMO"};
const char* kFinishes[] = {"ANODIZED", "BURNISHED", "PLATED", "POLISHED", "BRUSHED"};
const char* kMetals[] = {"TIN", "NICKEL", "BRASS", "STEEL", "COPPER"};
const char* kColors[] = {"almond",    "antique",   "aquamarine", "azure",      "beige",
                         "bisque",    "black",     "blanched",   "blue",       "blush",
                         "brown",     "burlywood", "burnished",  "chartreuse", "chiffon",
                         "chocolate", "coral",     "cornflower", "cornsilk",   "cream",
                         "cyan",      "dark",      "deep",       "dim"};
const char* kWords[] = {"final",        "regular",     "special",   "furious",   "pending",
                        "express",      "blithely",    "carefully", "quickly",   "slyly",
                        "even",         "bold",        "ironic",    "silent",    "busy",
                        "unusual",      "packages",    "requests",  "accounts",  "deposits",
                        "instructions", "theodolites", "excuses",   "asymptotes"};

template <typename T> void AppendNumber(std::string* out, T value) {
  char buffer[32];
  const int written = std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  out->append(buffer, static_cast<size_t>(written));
}

void AppendDecimal(std::string* out, int64_t units) {
  if (units < 0) {
    out->push_back('-');
    units = -units;
  }
  AppendNumber(out, units / 100);
  out->push_back('.');
  const int64_t rest = units % 100;
  out->push_back(static_cast<char>('0' + rest / 10));
  out->push_back(static_cast<char>('0' + rest % 10));
}

void AppendPhone(std::string* out, Random* random, int nation) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%02d-%03d-%03d-%04d", nation + 10,
                static_cast<int>(random->Between(100, 999)),
                static_cast<int>(random->Between(100, 999)),
                static_cast<int>(random->Between(1000, 9999)));
  out->append(buffer);
}

void AppendText(std::string* out, Random* random, int words) {
  for (int i = 0; i < words; ++i) {
    if (i > 0) {
      out->push_back(' ');
    }
    out->append(kWords[random->Next() % (sizeof(kWords) / sizeof(kWords[0]))]);
  }
}

void AppendPadded(std::string* out, const char* prefix, int64_t value, int width) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%s%0*lld", prefix, width, static_cast<long long>(value));
  out->append(buffer);
}

class TableFile {
public:
  TableFile(const std::string& path) : stream_(path, std::ios::binary | std::ios::trunc) {
    Check(stream_.good(), "cannot write to " + path);
    buffer_.reserve(kFlushSize + (1u << 16));
  }

  std::string& line() {
    return buffer_;
  }

  void EndRow() {
    buffer_.push_back('\n');
    ++rows_;
    if (buffer_.size() >= kFlushSize) {
      Flush();
    }
  }

  void Flush() {
    stream_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    Check(stream_.good(), "write failed while generating data");
    buffer_.clear();
  }

  uint64_t rows() const {
    return rows_;
  }

  ~TableFile() {
    if (!buffer_.empty()) {
      Flush();
    }
  }

private:
  static constexpr size_t kFlushSize = 4u << 20;
  std::ofstream stream_;
  std::string buffer_;
  uint64_t rows_ = 0;
};

}

std::vector<std::string> TpchTables() {
  return {"region", "nation", "supplier", "part", "partsupp", "customer", "orders", "lineitem"};
}

Schema TpchSchema(const std::string& table) {
  auto column = [](const char* name, DataType type, uint8_t scale = 0) {
    ColumnSchema entry;
    entry.name = name;
    entry.type = type;
    entry.scale = scale;
    return entry;
  };
  Schema schema;
  if (table == "region") {
    schema.Add(column("r_regionkey", DataType::kInt32));
    schema.Add(column("r_name", DataType::kString));
    schema.Add(column("r_comment", DataType::kString));
  } else if (table == "nation") {
    schema.Add(column("n_nationkey", DataType::kInt32));
    schema.Add(column("n_name", DataType::kString));
    schema.Add(column("n_regionkey", DataType::kInt32));
    schema.Add(column("n_comment", DataType::kString));
  } else if (table == "supplier") {
    schema.Add(column("s_suppkey", DataType::kInt32));
    schema.Add(column("s_name", DataType::kString));
    schema.Add(column("s_address", DataType::kString));
    schema.Add(column("s_nationkey", DataType::kInt32));
    schema.Add(column("s_phone", DataType::kString));
    schema.Add(column("s_acctbal", DataType::kDecimal, 2));
    schema.Add(column("s_comment", DataType::kString));
  } else if (table == "part") {
    schema.Add(column("p_partkey", DataType::kInt32));
    schema.Add(column("p_name", DataType::kString));
    schema.Add(column("p_mfgr", DataType::kString));
    schema.Add(column("p_brand", DataType::kString));
    schema.Add(column("p_type", DataType::kString));
    schema.Add(column("p_size", DataType::kInt32));
    schema.Add(column("p_container", DataType::kString));
    schema.Add(column("p_retailprice", DataType::kDecimal, 2));
    schema.Add(column("p_comment", DataType::kString));
  } else if (table == "partsupp") {
    schema.Add(column("ps_partkey", DataType::kInt32));
    schema.Add(column("ps_suppkey", DataType::kInt32));
    schema.Add(column("ps_availqty", DataType::kInt32));
    schema.Add(column("ps_supplycost", DataType::kDecimal, 2));
    schema.Add(column("ps_comment", DataType::kString));
  } else if (table == "customer") {
    schema.Add(column("c_custkey", DataType::kInt32));
    schema.Add(column("c_name", DataType::kString));
    schema.Add(column("c_address", DataType::kString));
    schema.Add(column("c_nationkey", DataType::kInt32));
    schema.Add(column("c_phone", DataType::kString));
    schema.Add(column("c_acctbal", DataType::kDecimal, 2));
    schema.Add(column("c_mktsegment", DataType::kString));
    schema.Add(column("c_comment", DataType::kString));
  } else if (table == "orders") {
    schema.Add(column("o_orderkey", DataType::kInt64));
    schema.Add(column("o_custkey", DataType::kInt32));
    schema.Add(column("o_orderstatus", DataType::kString));
    schema.Add(column("o_totalprice", DataType::kDecimal, 2));
    schema.Add(column("o_orderdate", DataType::kDate));
    schema.Add(column("o_orderpriority", DataType::kString));
    schema.Add(column("o_clerk", DataType::kString));
    schema.Add(column("o_shippriority", DataType::kInt32));
    schema.Add(column("o_comment", DataType::kString));
  } else if (table == "lineitem") {
    schema.Add(column("l_orderkey", DataType::kInt64));
    schema.Add(column("l_partkey", DataType::kInt32));
    schema.Add(column("l_suppkey", DataType::kInt32));
    schema.Add(column("l_linenumber", DataType::kInt32));
    schema.Add(column("l_quantity", DataType::kDecimal, 2));
    schema.Add(column("l_extendedprice", DataType::kDecimal, 2));
    schema.Add(column("l_discount", DataType::kDecimal, 2));
    schema.Add(column("l_tax", DataType::kDecimal, 2));
    schema.Add(column("l_returnflag", DataType::kString));
    schema.Add(column("l_linestatus", DataType::kString));
    schema.Add(column("l_shipdate", DataType::kDate));
    schema.Add(column("l_commitdate", DataType::kDate));
    schema.Add(column("l_receiptdate", DataType::kDate));
    schema.Add(column("l_shipinstruct", DataType::kString));
    schema.Add(column("l_shipmode", DataType::kString));
    schema.Add(column("l_comment", DataType::kString));
  } else {
    Fail("unknown tpch table: " + table);
  }
  return schema;
}

GenerateReport GenerateTpch(const std::string& directory, double scale) {
  Check(scale > 0.0 && scale <= 100.0, "scale must be between 0 and 100");
  Timer timer;
  GenerateReport report;
  const int32_t first_day = DaysFromCivil(1992, 1, 1);
  const int32_t cutoff = DaysFromCivil(1995, 6, 17);
  std::vector<std::string> date_text;
  date_text.reserve(4000);
  for (int32_t i = 0; i < 4000; ++i) {
    date_text.push_back(FormatDate(first_day + i));
  }
  const size_t nations = sizeof(kNations) / sizeof(kNations[0]);

  {
    TableFile file(directory + "/region.tbl");
    Random random(11);
    for (size_t i = 0; i < 5; ++i) {
      std::string& line = file.line();
      AppendNumber(&line, i);
      line.push_back('|');
      line.append(kRegions[i]);
      line.push_back('|');
      AppendText(&line, &random, 6);
      line.push_back('|');
      file.EndRow();
    }
    report.tables.emplace_back("region", file.rows());
  }
  {
    TableFile file(directory + "/nation.tbl");
    Random random(13);
    for (size_t i = 0; i < nations; ++i) {
      std::string& line = file.line();
      AppendNumber(&line, i);
      line.push_back('|');
      line.append(kNations[i].name);
      line.push_back('|');
      AppendNumber(&line, kNations[i].region);
      line.push_back('|');
      AppendText(&line, &random, 6);
      line.push_back('|');
      file.EndRow();
    }
    report.tables.emplace_back("nation", file.rows());
  }
  {
    const int64_t count = static_cast<int64_t>(10000 * scale);
    TableFile file(directory + "/supplier.tbl");
    Random random(17);
    for (int64_t i = 1; i <= count; ++i) {
      const int nation = static_cast<int>(random.Next() % nations);
      std::string& line = file.line();
      AppendNumber(&line, i);
      line.push_back('|');
      AppendPadded(&line, "Supplier#", i, 9);
      line.push_back('|');
      AppendText(&line, &random, 3);
      line.push_back('|');
      AppendNumber(&line, nation);
      line.push_back('|');
      AppendPhone(&line, &random, nation);
      line.push_back('|');
      AppendDecimal(&line, random.Between(-99999, 999999));
      line.push_back('|');
      AppendText(&line, &random, 8);
      line.push_back('|');
      file.EndRow();
    }
    report.tables.emplace_back("supplier", file.rows());
  }
  {
    const int64_t count = static_cast<int64_t>(200000 * scale);
    TableFile file(directory + "/part.tbl");
    Random random(19);
    for (int64_t i = 1; i <= count; ++i) {
      std::string& line = file.line();
      AppendNumber(&line, i);
      line.push_back('|');
      for (int word = 0; word < 5; ++word) {
        if (word > 0) {
          line.push_back(' ');
        }
        line.append(kColors[random.Next() % (sizeof(kColors) / sizeof(kColors[0]))]);
      }
      line.push_back('|');
      AppendPadded(&line, "Manufacturer#", random.Between(1, 5), 1);
      line.push_back('|');
      AppendPadded(&line, "Brand#", random.Between(11, 55), 2);
      line.push_back('|');
      line.append(kMaterials[random.Next() % (sizeof(kMaterials) / sizeof(kMaterials[0]))]);
      line.push_back(' ');
      line.append(kFinishes[random.Next() % (sizeof(kFinishes) / sizeof(kFinishes[0]))]);
      line.push_back(' ');
      line.append(kMetals[random.Next() % (sizeof(kMetals) / sizeof(kMetals[0]))]);
      line.push_back('|');
      AppendNumber(&line, random.Between(1, 50));
      line.push_back('|');
      line.append(kContainers[random.Next() % (sizeof(kContainers) / sizeof(kContainers[0]))]);
      line.push_back('|');
      AppendDecimal(&line, 90000 + (i % 100000) + 100 * (i % 1000));
      line.push_back('|');
      AppendText(&line, &random, 5);
      line.push_back('|');
      file.EndRow();
    }
    report.tables.emplace_back("part", file.rows());
  }
  {
    const int64_t parts = static_cast<int64_t>(200000 * scale);
    const int64_t suppliers = static_cast<int64_t>(10000 * scale);
    TableFile file(directory + "/partsupp.tbl");
    Random random(23);
    for (int64_t part = 1; part <= parts; ++part) {
      for (int64_t copy = 0; copy < 4; ++copy) {
        std::string& line = file.line();
        AppendNumber(&line, part);
        line.push_back('|');
        AppendNumber(&line, ((part + copy * (suppliers / 4)) % suppliers) + 1);
        line.push_back('|');
        AppendNumber(&line, random.Between(1, 9999));
        line.push_back('|');
        AppendDecimal(&line, random.Between(100, 100000));
        line.push_back('|');
        AppendText(&line, &random, 10);
        line.push_back('|');
        file.EndRow();
      }
    }
    report.tables.emplace_back("partsupp", file.rows());
  }
  {
    const int64_t count = static_cast<int64_t>(150000 * scale);
    TableFile file(directory + "/customer.tbl");
    Random random(29);
    for (int64_t i = 1; i <= count; ++i) {
      const int nation = static_cast<int>(random.Next() % nations);
      std::string& line = file.line();
      AppendNumber(&line, i);
      line.push_back('|');
      AppendPadded(&line, "Customer#", i, 9);
      line.push_back('|');
      AppendText(&line, &random, 3);
      line.push_back('|');
      AppendNumber(&line, nation);
      line.push_back('|');
      AppendPhone(&line, &random, nation);
      line.push_back('|');
      AppendDecimal(&line, random.Between(-99999, 999999));
      line.push_back('|');
      line.append(kSegments[random.Next() % (sizeof(kSegments) / sizeof(kSegments[0]))]);
      line.push_back('|');
      AppendText(&line, &random, 9);
      line.push_back('|');
      file.EndRow();
    }
    report.tables.emplace_back("customer", file.rows());
  }
  {
    const int64_t orders = static_cast<int64_t>(1500000 * scale);
    const int64_t customers = static_cast<int64_t>(150000 * scale);
    const int64_t parts = static_cast<int64_t>(200000 * scale);
    const int64_t suppliers = static_cast<int64_t>(10000 * scale);
    TableFile order_file(directory + "/orders.tbl");
    TableFile line_file(directory + "/lineitem.tbl");
    Random random(31);
    for (int64_t index = 1; index <= orders; ++index) {
      const int64_t order_key = index * 4 - 3;
      const int32_t order_date = static_cast<int32_t>(random.Between(0, 2405));
      const int64_t lines = random.Between(1, 7);
      int64_t total = 0;
      std::string& line = line_file.line();
      for (int64_t number = 1; number <= lines; ++number) {
        const int64_t quantity = random.Between(1, 50) * 100;
        const int64_t part_key = random.Between(1, parts);
        const int64_t price =
            (90000 + (part_key % 100000) + 100 * (part_key % 1000)) * quantity / 100;
        const int64_t discount = random.Between(0, 10);
        const int64_t tax = random.Between(0, 8);
        const int32_t ship = order_date + static_cast<int32_t>(random.Between(1, 121));
        const int32_t commit = order_date + static_cast<int32_t>(random.Between(30, 90));
        const int32_t receipt = ship + static_cast<int32_t>(random.Between(1, 30));
        total += price * (100 - discount) / 100;
        AppendNumber(&line, order_key);
        line.push_back('|');
        AppendNumber(&line, part_key);
        line.push_back('|');
        AppendNumber(&line, random.Between(1, suppliers));
        line.push_back('|');
        AppendNumber(&line, number);
        line.push_back('|');
        AppendDecimal(&line, quantity);
        line.push_back('|');
        AppendDecimal(&line, price);
        line.push_back('|');
        AppendDecimal(&line, discount);
        line.push_back('|');
        AppendDecimal(&line, tax);
        line.push_back('|');
        if (first_day + receipt <= cutoff) {
          line.push_back(random.Next() % 2 == 0 ? 'R' : 'A');
        } else {
          line.push_back('N');
        }
        line.push_back('|');
        line.push_back(first_day + order_date > cutoff ? 'O' : 'F');
        line.push_back('|');
        line.append(date_text[static_cast<size_t>(ship)]);
        line.push_back('|');
        line.append(date_text[static_cast<size_t>(commit)]);
        line.push_back('|');
        line.append(date_text[static_cast<size_t>(receipt)]);
        line.push_back('|');
        line.append(
            kInstructions[random.Next() % (sizeof(kInstructions) / sizeof(kInstructions[0]))]);
        line.push_back('|');
        line.append(kModes[random.Next() % (sizeof(kModes) / sizeof(kModes[0]))]);
        line.push_back('|');
        AppendText(&line, &random, 4);
        line.push_back('|');
        line_file.EndRow();
      }
      std::string& order_line = order_file.line();
      AppendNumber(&order_line, order_key);
      order_line.push_back('|');
      AppendNumber(&order_line, random.Between(1, customers));
      order_line.push_back('|');
      order_line.push_back(first_day + order_date > cutoff ? 'O' : 'F');
      order_line.push_back('|');
      AppendDecimal(&order_line, total);
      order_line.push_back('|');
      order_line.append(date_text[static_cast<size_t>(order_date)]);
      order_line.push_back('|');
      order_line.append(
          kPriorities[random.Next() % (sizeof(kPriorities) / sizeof(kPriorities[0]))]);
      order_line.push_back('|');
      AppendPadded(&order_line, "Clerk#", random.Between(1, 1000), 9);
      order_line.push_back('|');
      order_line.push_back('0');
      order_line.push_back('|');
      AppendText(&order_line, &random, 7);
      order_line.push_back('|');
      order_file.EndRow();
    }
    report.tables.emplace_back("orders", order_file.rows());
    report.tables.emplace_back("lineitem", line_file.rows());
  }
  report.seconds = timer.Seconds();
  return report;
}

}
