#pragma once

#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "hash_table.h"
#include "parser.h"
#include "reader.h"
#include "types.h"

namespace vcqe {

struct ProjectionItem {
  ExprPtr expr;
  std::string name;
  bool hidden = false;
};

struct OrderItem {
  size_t column = 0;
  bool descending = false;
};

struct ScanSpec {
  std::string alias;
  std::string table;
  std::string path;
  std::vector<size_t> source_columns;
  Schema output;
  ExprPtr filter;
  uint64_t rows = 0;
};

struct PipelinePlan;
using PipelinePtr = std::shared_ptr<PipelinePlan>;

struct JoinSpec {
  PipelinePtr build;
  std::vector<ExprPtr> probe_keys;
  std::vector<ExprPtr> build_keys;
  std::vector<size_t> payload;
  std::vector<ColumnSchema> payload_schema;
  Schema output;
  ExprPtr residual;
  uint64_t build_rows = 0;
};

struct PipelinePlan {
  ScanSpec scan;
  std::vector<JoinSpec> joins;
  Schema output;
  int slots = 0;
};

struct QueryPlan {
  PipelinePlan pipeline;
  bool has_aggregate = false;
  std::vector<ExprPtr> group_keys;
  std::vector<AggregateSpec> aggregates;
  Schema post_schema;
  ExprPtr having;
  std::vector<ProjectionItem> projections;
  std::vector<OrderItem> order;
  int64_t limit = -1;
  Schema result_schema;
  int slots = 0;
};

class Catalog {
public:
  explicit Catalog(std::string directory) : directory_(std::move(directory)) {}
  const std::string& directory() const {
    return directory_;
  }
  std::string PathFor(const std::string& table) const;
  bool Exists(const std::string& table) const;
  const TableReader& Open(const std::string& table);
  std::vector<std::string> Tables() const;

private:
  std::string directory_;
  std::vector<std::pair<std::string, std::shared_ptr<TableReader>>> open_;
};

QueryPlan BuildPlan(const SelectStatement& statement, Catalog* catalog, const Config& config);
std::string DescribePlan(const QueryPlan& plan);

}
