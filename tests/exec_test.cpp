#include <string>
#include <vector>

#include "dataset.h"
#include "engine.h"
#include "harness.h"
#include "util.h"

using namespace vcqe;

namespace {

std::string Render(const QueryResult& result) {
  std::string text;
  for (const std::vector<Value>& row : result.rows) {
    for (size_t i = 0; i < row.size(); ++i) {
      if (i > 0) {
        text += "|";
      }
      text += FormatValue(row[i]);
    }
    text += "\n";
  }
  return text;
}

std::string Run(const std::string& sql, Config config) {
  Engine engine(config, SmallDataset());
  return Render(engine.Query(sql).result);
}

void ExpectSameEverywhere(const std::string& sql) {
  Config vectorized;
  const std::string expected = Run(sql, vectorized);

  Config row;
  row.execution.mode = "row";
  EXPECT_EQ(Run(sql, row), expected);

  Config single;
  single.execution.vector_size = 1;
  EXPECT_EQ(Run(sql, single), expected);

  Config odd;
  odd.execution.vector_size = 7;
  odd.execution.threads = 3;
  EXPECT_EQ(Run(sql, odd), expected);

  Config plain;
  plain.execution.enable_simd = false;
  plain.execution.enable_zone_maps = false;
  plain.execution.enable_predicate_pushdown = false;
  plain.execution.enable_join_reorder = false;
  EXPECT_EQ(Run(sql, plain), expected);
}

}

TEST(ScanAndFilter) {
  ExpectSameEverywhere("select count(*) from lineitem");
  ExpectSameEverywhere("select count(*) from lineitem where l_quantity > 30");
  ExpectSameEverywhere("select count(*) from lineitem where l_shipdate < date '1994-01-01'");
  ExpectSameEverywhere(
      "select count(*) from lineitem where l_returnflag = 'R' and l_discount >= 0.05");
  ExpectSameEverywhere(
      "select count(*) from lineitem where l_returnflag = 'R' or l_linestatus = 'F'");
  ExpectSameEverywhere("select count(*) from lineitem where not l_returnflag = 'R'");
  ExpectSameEverywhere("select count(*) from part where p_name like '%almond%'");
}

TEST(AggregatesAndGrouping) {
  ExpectSameEverywhere(
      "select l_returnflag, count(*), sum(l_quantity), min(l_discount), max(l_discount), "
      "avg(l_extendedprice) from lineitem group by l_returnflag order by l_returnflag");
  ExpectSameEverywhere(
      "select l_returnflag, l_linestatus, sum(l_extendedprice * (1 - l_discount)) as revenue "
      "from lineitem group by l_returnflag, l_linestatus order by revenue desc");
  ExpectSameEverywhere("select sum(l_quantity) from lineitem where l_orderkey < 0");
  ExpectSameEverywhere("select count(*) from lineitem where l_orderkey < 0");
  ExpectSameEverywhere(
      "select l_returnflag, count(*) as n from lineitem group by l_returnflag having count(*) > 1 "
      "order by n desc");
}

TEST(JoinsMatchAcrossModes) {
  ExpectSameEverywhere("select count(*) from customer, orders where c_custkey = o_custkey");
  ExpectSameEverywhere(
      "select n_name, count(*) as n from supplier, nation where s_nationkey = n_nationkey "
      "group by n_name order by n_name");
  for (const char* name : {"q1", "q3", "q5", "q6", "q10"}) {
    ExpectSameEverywhere(ReadTextFile(std::string("queries/") + name + ".sql"));
  }
}

TEST(OrderAndLimit) {
  ExpectSameEverywhere(
      "select l_orderkey, l_extendedprice from lineitem order by l_extendedprice desc, l_orderkey "
      "limit 5");
  ExpectSameEverywhere(
      "select o_orderkey from orders where o_orderdate < date '1993-01-01' order by o_orderkey "
      "limit 3");
}

TEST(ZoneMapsDoNotChangeResults) {
  Config with;
  Config without;
  without.execution.enable_zone_maps = false;
  const std::string sql =
      "select count(*), sum(l_extendedprice) from lineitem where l_orderkey between 1000 and 2000";
  EXPECT_EQ(Run(sql, with), Run(sql, without));
}

TEST(ZoneMapsSkipRowGroups) {
  Config config;
  Engine engine(config, SmallDataset());
  const QueryRun run =
      engine.Query("select count(*) from lineitem where l_orderkey between 1 and 100");
  EXPECT(run.result.stats.row_groups_skipped > 0);
  EXPECT(run.result.stats.rows_scanned < 30000);
}

TEST(ExplainDescribesThePlan) {
  Config config;
  Engine engine(config, SmallDataset());
  const std::string text = engine.Explain(ReadTextFile("queries/q3.sql"));
  EXPECT(text.find("scan lineitem") != std::string::npos);
  EXPECT(text.find("hash join") != std::string::npos);
  EXPECT(text.find("filter") != std::string::npos);
}

TEST(ReportsUsefulErrors) {
  Config config;
  Engine engine(config, SmallDataset());
  EXPECT_THROWS(engine.Query("select nope from lineitem"));
  EXPECT_THROWS(engine.Query("select count(*) from lineitem, orders"));
  EXPECT_THROWS(engine.Query("select count(*) from nothing"));
}

TEST_MAIN()
