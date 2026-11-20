#include "dataset.h"
#include "harness.h"
#include "parser.h"
#include "plan.h"
#include "util.h"

using namespace vcqe;

namespace {

QueryPlan Plan(const std::string& sql, Config config = Config()) {
  static Catalog catalog(SmallDataset());
  const SelectStatement statement = ParseSelect(sql);
  return BuildPlan(statement, &catalog, config);
}

}

TEST(ParsesTheBasicShape) {
  const SelectStatement statement = ParseSelect(
      "select a, sum(b) as total from t where a > 3 and b <= 4 group by a having sum(b) > 1 "
      "order by total desc limit 5");
  EXPECT_EQ(statement.items.size(), 2u);
  EXPECT_EQ(statement.items[1].alias, std::string("total"));
  EXPECT_EQ(statement.tables.size(), 1u);
  EXPECT_EQ(statement.group_by.size(), 1u);
  EXPECT(statement.having != nullptr);
  EXPECT_EQ(statement.order_by.size(), 1u);
  EXPECT(statement.order_by[0].descending);
  EXPECT_EQ(statement.limit, 5);
}

TEST(FoldsDateIntervals) {
  const SelectStatement statement =
      ParseSelect("select a from t where d < date '1994-01-01' + interval '1' year");
  const ExprPtr right = statement.where->children[1];
  EXPECT_EQ(right->kind, ExprKind::kConstant);
  EXPECT_EQ(FormatValue(right->constant), std::string("1995-01-01"));

  const SelectStatement second =
      ParseSelect("select a from t where d <= date '1998-12-01' - interval '90' day");
  EXPECT_EQ(FormatValue(second.where->children[1]->constant), std::string("1998-09-02"));

  const SelectStatement third =
      ParseSelect("select a from t where d < date '1993-10-01' + interval '3' month");
  EXPECT_EQ(FormatValue(third.where->children[1]->constant), std::string("1994-01-01"));
}

TEST(ExpandsBetweenAndIn) {
  const SelectStatement statement = ParseSelect("select a from t where a between 1 and 4");
  EXPECT_EQ(statement.where->kind, ExprKind::kLogical);
  EXPECT_EQ(statement.where->children.size(), 2u);
  const SelectStatement second = ParseSelect("select a from t where a in (1, 2, 3)");
  EXPECT_EQ(second.where->children.size(), 3u);
}

TEST(RejectsBadSql) {
  EXPECT_THROWS(ParseSelect("update t set a = 1"));
  EXPECT_THROWS(ParseSelect("select a from"));
  EXPECT_THROWS(ParseSelect("select from t"));
  EXPECT_THROWS(ParseSelect("select a from t where"));
  EXPECT_THROWS(ParseSelect("select count(distinct a) from t"));
  EXPECT_THROWS(ParseSelect("select median(a) from t"));
  EXPECT_THROWS(ParseSelect("select a from t limit"));
  EXPECT_THROWS(ParseSelect("select a from t extra text"));
}

TEST(PlannerPushesPredicatesDown) {
  const QueryPlan plan = Plan("select l_orderkey from lineitem where l_quantity > 30");
  EXPECT(plan.pipeline.scan.filter != nullptr);
  EXPECT_EQ(plan.pipeline.joins.size(), 0u);
}

TEST(PlannerPrunesColumns) {
  const QueryPlan plan = Plan("select count(*) from lineitem where l_quantity > 30");
  EXPECT_EQ(plan.pipeline.scan.output.size(), 1u);
  EXPECT_EQ(plan.pipeline.scan.output.at(0).name, std::string("l_quantity"));
}

TEST(PlannerPicksTheLargestTableToProbe) {
  const QueryPlan plan = Plan("select count(*) from customer, orders where c_custkey = o_custkey");
  EXPECT_EQ(plan.pipeline.scan.table, std::string("orders"));
  EXPECT_EQ(plan.pipeline.joins.size(), 1u);
  EXPECT_EQ(plan.pipeline.joins[0].build->scan.table, std::string("customer"));
}

TEST(PlannerAvoidsTheLowCardinalityJoinFirst) {
  const std::string sql = ReadTextFile("queries/q5.sql");
  const QueryPlan plan = Plan(sql);
  EXPECT_EQ(plan.pipeline.scan.table, std::string("lineitem"));
  EXPECT_EQ(plan.pipeline.joins.size(), 5u);
  const std::string last = plan.pipeline.joins.back().build->scan.table;
  EXPECT_EQ(last, std::string("customer"));
  EXPECT_EQ(plan.pipeline.joins.back().build_keys.size(), 2u);
}

TEST(PlannerKeepsFromOrderWhenReorderIsOff) {
  Config config;
  config.execution.enable_join_reorder = false;
  const QueryPlan plan =
      Plan("select count(*) from customer, orders where c_custkey = o_custkey", config);
  EXPECT_EQ(plan.pipeline.scan.table, std::string("customer"));
}

TEST(PlansEveryBenchmarkQuery) {
  for (const char* name : {"q1", "q3", "q5", "q6", "q10"}) {
    const QueryPlan plan = Plan(ReadTextFile(std::string("queries/") + name + ".sql"));
    EXPECT(plan.has_aggregate);
    EXPECT(plan.result_schema.size() > 0);
    EXPECT(!DescribePlan(plan).empty());
  }
}

TEST(RejectsUnknownNames) {
  EXPECT_THROWS(Plan("select nope from lineitem"));
  EXPECT_THROWS(Plan("select l_orderkey from missing_table"));
  EXPECT_THROWS(Plan("select count(*) from lineitem, orders"));
}

TEST_MAIN()
