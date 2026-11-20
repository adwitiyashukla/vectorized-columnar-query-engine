#include "plan.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "error.h"
#include "util.h"

namespace vcqe {
namespace {

struct TableEntry {
  std::string alias;
  std::string name;
  const TableReader* reader = nullptr;
  uint64_t rows = 0;
  std::vector<bool> needed;
  std::vector<std::string> display;
};

struct Conjunct {
  ExprPtr expr;
  std::set<size_t> tables;
  bool used = false;
};

void Flatten(const ExprPtr& expr, std::vector<ExprPtr>* out) {
  if (expr == nullptr) {
    return;
  }
  if (expr->kind == ExprKind::kLogical && expr->logical == LogicalOp::kAnd) {
    for (const ExprPtr& child : expr->children) {
      Flatten(child, out);
    }
    return;
  }
  out->push_back(expr);
}

ExprPtr Combine(const std::vector<ExprPtr>& parts) {
  if (parts.empty()) {
    return nullptr;
  }
  if (parts.size() == 1) {
    return parts[0];
  }
  return MakeLogical(LogicalOp::kAnd, parts);
}

bool ExprEqual(const Expr& left, const Expr& right) {
  if (left.kind != right.kind || left.children.size() != right.children.size()) {
    return false;
  }
  if (left.kind == ExprKind::kColumn && left.column_name != right.column_name) {
    return false;
  }
  if (left.kind == ExprKind::kConstant &&
      FormatValue(left.constant) != FormatValue(right.constant)) {
    return false;
  }
  if (left.kind == ExprKind::kArithmetic && left.arith != right.arith) {
    return false;
  }
  if (left.kind == ExprKind::kCompare && left.compare != right.compare) {
    return false;
  }
  for (size_t i = 0; i < left.children.size(); ++i) {
    if (!ExprEqual(*left.children[i], *right.children[i])) {
      return false;
    }
  }
  return true;
}

class Planner {
public:
  Planner(const SelectStatement& statement, Catalog* catalog, const Config& config)
      : statement_(statement), catalog_(catalog), config_(config) {}

  QueryPlan Build() {
    order_terms_ = statement_.order_by;
    group_terms_ = statement_.group_by;
    LoadTables();
    ExpandStar();
    ResolveAllColumns();
    SplitPredicates();
    QueryPlan plan;
    plan.pipeline = BuildPipeline();
    BuildAggregates(&plan);
    BuildProjections(&plan);
    plan.limit = statement_.limit;
    plan.slots = slots_;
    plan.pipeline.slots = slots_;
    SetSlots(&plan.pipeline);
    return plan;
  }

private:
  void SetSlots(PipelinePlan* pipeline) {
    pipeline->slots = slots_;
    for (JoinSpec& join : pipeline->joins) {
      SetSlots(join.build.get());
    }
  }

  void LoadTables() {
    for (const TableRef& reference : statement_.tables) {
      TableEntry entry;
      entry.alias = reference.alias;
      entry.name = reference.name;
      entry.reader = &catalog_->Open(reference.name);
      entry.rows = entry.reader->rows();
      entry.needed.assign(entry.reader->schema().size(), false);
      tables_.push_back(std::move(entry));
    }
    Check(!tables_.empty(), "query needs at least one table");
    std::map<std::string, int> counts;
    for (const TableEntry& entry : tables_) {
      for (const ColumnSchema& column : entry.reader->schema().columns()) {
        ++counts[column.name];
      }
    }
    for (TableEntry& entry : tables_) {
      for (const ColumnSchema& column : entry.reader->schema().columns()) {
        entry.display.push_back(counts[column.name] > 1 ? entry.alias + "." + column.name
                                                        : column.name);
      }
    }
  }

  void ExpandStar() {
    std::vector<SelectItem> expanded;
    for (const SelectItem& item : statement_.items) {
      if (!item.star) {
        expanded.push_back(item);
        continue;
      }
      for (size_t t = 0; t < tables_.size(); ++t) {
        for (const ColumnSchema& column : tables_[t].reader->schema().columns()) {
          SelectItem single;
          single.expr = MakeColumn(tables_[t].alias, column.name);
          expanded.push_back(std::move(single));
        }
      }
    }
    items_ = std::move(expanded);
  }

  size_t ResolveTable(const std::string& alias, const std::string& column) {
    if (!alias.empty()) {
      for (size_t t = 0; t < tables_.size(); ++t) {
        if (tables_[t].alias == alias) {
          Check(tables_[t].reader->schema().IndexOf(column) >= 0,
                "unknown column " + column + " in table " + alias);
          return t;
        }
      }
      Fail("unknown table alias: " + alias);
    }
    int found = -1;
    for (size_t t = 0; t < tables_.size(); ++t) {
      if (tables_[t].reader->schema().IndexOf(column) >= 0) {
        Check(found < 0, "column " + column + " is in more than one table");
        found = static_cast<int>(t);
      }
    }
    Check(found >= 0, "unknown column: " + column);
    return static_cast<size_t>(found);
  }

  void ResolveColumns(const ExprPtr& expr) {
    if (expr == nullptr) {
      return;
    }
    if (expr->kind == ExprKind::kColumn && expr->column_name.rfind("agg_", 0) != 0 &&
        aliases_.count(expr->column_name) == 0) {
      const size_t table = ResolveTable(expr->table_name, expr->column_name);
      const size_t column =
          static_cast<size_t>(tables_[table].reader->schema().IndexOf(expr->column_name));
      tables_[table].needed[column] = true;
      expr->table_name.clear();
      expr->column_name = tables_[table].display[column];
      column_owner_[expr->column_name] = table;
    }
    for (const ExprPtr& child : expr->children) {
      ResolveColumns(child);
    }
  }

  void ResolveAllColumns() {
    for (const SelectItem& item : items_) {
      if (!item.alias.empty()) {
        aliases_.insert(ToLower(item.alias));
      }
    }
    for (SelectItem& item : items_) {
      ResolveColumns(item.expr);
    }
    ResolveColumns(statement_.where);
    for (const ExprPtr& key : group_terms_) {
      ResolveColumns(key);
    }
    ResolveColumns(statement_.having);
    for (const OrderTerm& term : order_terms_) {
      ResolveColumns(term.expr);
    }
  }

  std::set<size_t> TablesOf(const ExprPtr& expr) {
    std::set<size_t> used;
    CollectTables(expr, &used);
    return used;
  }

  void CollectTables(const ExprPtr& expr, std::set<size_t>* used) {
    if (expr->kind == ExprKind::kColumn) {
      const auto found = column_owner_.find(expr->column_name);
      if (found != column_owner_.end()) {
        used->insert(found->second);
      }
    }
    for (const ExprPtr& child : expr->children) {
      CollectTables(child, used);
    }
  }

  void SplitPredicates() {
    std::vector<ExprPtr> parts;
    Flatten(statement_.where, &parts);
    for (const ExprPtr& part : parts) {
      Conjunct conjunct;
      conjunct.expr = part;
      conjunct.tables = TablesOf(part);
      if (conjunct.tables.size() == 2 && part->kind == ExprKind::kCompare &&
          part->compare == CompareOp::kEqual && part->children[0]->kind == ExprKind::kColumn &&
          part->children[1]->kind == ExprKind::kColumn) {
        edges_.push_back(conjunct);
        continue;
      }
      if (conjunct.tables.size() == 1) {
        pushed_[*conjunct.tables.begin()].push_back(part);
        continue;
      }
      residuals_.push_back(conjunct);
    }
  }

  Schema ScanSchema(size_t table, std::vector<size_t>* source_columns) {
    Schema schema;
    const TableEntry& entry = tables_[table];
    for (size_t c = 0; c < entry.reader->schema().size(); ++c) {
      if (!entry.needed[c]) {
        continue;
      }
      ColumnSchema column = entry.reader->schema().at(c);
      column.name = entry.display[c];
      schema.Add(column);
      source_columns->push_back(c);
    }
    if (schema.size() == 0) {
      ColumnSchema column = entry.reader->schema().at(0);
      column.name = entry.display[0];
      schema.Add(column);
      source_columns->push_back(0);
    }
    return schema;
  }

  PipelinePtr BuildScanPipeline(size_t table) {
    auto pipeline = std::make_shared<PipelinePlan>();
    pipeline->scan.alias = tables_[table].alias;
    pipeline->scan.table = tables_[table].name;
    pipeline->scan.path = catalog_->PathFor(tables_[table].name);
    pipeline->scan.rows = tables_[table].rows;
    pipeline->scan.output = ScanSchema(table, &pipeline->scan.source_columns);
    pipeline->output = pipeline->scan.output;
    if (config_.execution.enable_predicate_pushdown) {
      const auto found = pushed_.find(table);
      if (found != pushed_.end()) {
        pipeline->scan.filter = Combine(found->second);
        BindExpr(*pipeline->scan.filter, pipeline->scan.output, &slots_);
      }
    }
    return pipeline;
  }

  uint64_t ColumnNdv(size_t table, const std::string& display) {
    const TableEntry& entry = tables_[table];
    size_t column = 0;
    bool found = false;
    for (size_t i = 0; i < entry.display.size(); ++i) {
      if (entry.display[i] == display) {
        column = i;
        found = true;
        break;
      }
    }
    if (!found) {
      return 1;
    }
    uint64_t total = 0;
    for (const RowGroupMeta& group : entry.reader->meta().row_groups) {
      total += group.chunks[column].distinct_count;
    }
    if (total == 0) {
      return 1;
    }
    return total < entry.rows ? total : entry.rows;
  }

  uint64_t EdgeNdv(const Conjunct& edge, size_t table) {
    uint64_t best = 1;
    for (const ExprPtr& child : edge.expr->children) {
      const auto owner = column_owner_.find(child->column_name);
      if (owner != column_owner_.end() && owner->second == table) {
        const uint64_t ndv = ColumnNdv(table, child->column_name);
        best = ndv > best ? ndv : best;
      }
    }
    return best;
  }

  PipelinePlan BuildPipeline() {
    size_t base = 0;
    if (config_.execution.enable_join_reorder) {
      for (size_t t = 1; t < tables_.size(); ++t) {
        if (tables_[t].rows > tables_[base].rows) {
          base = t;
        }
      }
    }
    if (!config_.execution.enable_predicate_pushdown) {
      for (const auto& entry : pushed_) {
        for (const ExprPtr& part : entry.second) {
          Conjunct conjunct;
          conjunct.expr = part;
          conjunct.tables = {entry.first};
          residuals_.push_back(conjunct);
        }
      }
      pushed_.clear();
    }
    PipelinePlan pipeline = *BuildScanPipeline(base);
    std::set<size_t> joined{base};
    Schema current = pipeline.output;
    while (joined.size() < tables_.size()) {
      int chosen = -1;
      double best_score = 0.0;
      for (size_t t = 0; t < tables_.size(); ++t) {
        if (joined.count(t) > 0) {
          continue;
        }
        uint64_t key_ndv = 0;
        for (const Conjunct& edge : edges_) {
          const auto it = edge.tables.begin();
          const size_t left = *it;
          const size_t right = *std::next(it);
          if (!((left == t && joined.count(right) > 0) || (right == t && joined.count(left) > 0))) {
            continue;
          }
          const uint64_t ndv = EdgeNdv(edge, t);
          key_ndv = ndv > key_ndv ? ndv : key_ndv;
        }
        if (key_ndv == 0) {
          continue;
        }
        const double score = static_cast<double>(tables_[t].rows) / static_cast<double>(key_ndv);
        if (chosen < 0) {
          chosen = static_cast<int>(t);
          best_score = score;
        } else if (config_.execution.enable_join_reorder &&
                   (score < best_score ||
                    (score == best_score &&
                     tables_[t].rows < tables_[static_cast<size_t>(chosen)].rows))) {
          chosen = static_cast<int>(t);
          best_score = score;
        }
      }
      Check(chosen >= 0, "every table needs a join condition to the rest of the query");
      const size_t table = static_cast<size_t>(chosen);
      JoinSpec join;
      join.build = BuildScanPipeline(table);
      join.build_rows = tables_[table].rows;
      for (Conjunct& edge : edges_) {
        if (edge.used) {
          continue;
        }
        const auto it = edge.tables.begin();
        const size_t left = *it;
        const size_t right = *std::next(it);
        if (!((left == table && joined.count(right) > 0) ||
              (right == table && joined.count(left) > 0))) {
          continue;
        }
        edge.used = true;
        ExprPtr build_side = edge.expr->children[0];
        ExprPtr probe_side = edge.expr->children[1];
        if (column_owner_[build_side->column_name] != table) {
          std::swap(build_side, probe_side);
        }
        ExprPtr build_key = CloneExpr(build_side);
        ExprPtr probe_key = CloneExpr(probe_side);
        BindExpr(*build_key, join.build->output, &slots_);
        BindExpr(*probe_key, current, &slots_);
        join.build_keys.push_back(build_key);
        join.probe_keys.push_back(probe_key);
      }
      Schema output = current;
      for (size_t c = 0; c < join.build->output.size(); ++c) {
        join.payload.push_back(c);
        join.payload_schema.push_back(join.build->output.at(c));
        output.Add(join.build->output.at(c));
      }
      join.output = output;
      joined.insert(table);
      std::vector<ExprPtr> ready;
      for (Conjunct& conjunct : residuals_) {
        if (conjunct.used) {
          continue;
        }
        bool available = true;
        for (size_t needed : conjunct.tables) {
          if (joined.count(needed) == 0) {
            available = false;
            break;
          }
        }
        if (!available) {
          continue;
        }
        conjunct.used = true;
        ExprPtr clone = CloneExpr(conjunct.expr);
        BindExpr(*clone, output, &slots_);
        ready.push_back(clone);
      }
      join.residual = Combine(ready);
      current = output;
      pipeline.joins.push_back(std::move(join));
    }
    std::vector<ExprPtr> leftover;
    for (Conjunct& conjunct : residuals_) {
      if (conjunct.used) {
        continue;
      }
      conjunct.used = true;
      ExprPtr clone = CloneExpr(conjunct.expr);
      BindExpr(*clone, current, &slots_);
      leftover.push_back(clone);
    }
    if (!leftover.empty()) {
      if (pipeline.joins.empty()) {
        std::vector<ExprPtr> parts;
        if (pipeline.scan.filter != nullptr) {
          parts.push_back(pipeline.scan.filter);
        }
        for (const ExprPtr& part : leftover) {
          parts.push_back(part);
        }
        pipeline.scan.filter = Combine(parts);
      } else {
        std::vector<ExprPtr> parts;
        if (pipeline.joins.back().residual != nullptr) {
          parts.push_back(pipeline.joins.back().residual);
        }
        for (const ExprPtr& part : leftover) {
          parts.push_back(part);
        }
        pipeline.joins.back().residual = Combine(parts);
      }
    }
    pipeline.output = current;
    return pipeline;
  }

  void CollectAggregates(const ExprPtr& expr, const Schema& input,
                         std::vector<AggregateSpec>* specs) {
    if (expr == nullptr) {
      return;
    }
    for (ExprPtr& child : expr->children) {
      if (child->kind == ExprKind::kAggregate) {
        AggregateSpec spec;
        spec.kind = static_cast<AggregateKind>(child->aggregate);
        spec.name = "agg_" + std::to_string(specs->size());
        if (!child->children.empty()) {
          spec.input = CloneExpr(child->children[0]);
          BindExpr(*spec.input, input, &slots_);
          spec.input_type = spec.input->type;
          spec.input_scale = spec.input->scale;
        }
        switch (spec.kind) {
        case AggregateKind::kCountStar:
        case AggregateKind::kCount:
          spec.output_type = DataType::kInt64;
          break;
        case AggregateKind::kAvg:
          spec.output_type = DataType::kDouble;
          break;
        case AggregateKind::kSum:
          spec.output_type =
              spec.input_type == DataType::kDate ? DataType::kInt64 : spec.input_type;
          spec.output_scale = spec.input_scale;
          break;
        case AggregateKind::kMin:
        case AggregateKind::kMax:
          spec.output_type = spec.input_type;
          spec.output_scale = spec.input_scale;
          break;
        }
        specs->push_back(spec);
        child = MakeColumn("", spec.name);
        continue;
      }
      CollectAggregates(child, input, specs);
    }
  }

  void BuildAggregates(QueryPlan* plan) {
    std::vector<AggregateSpec> specs;
    for (SelectItem& item : items_) {
      if (item.expr->kind == ExprKind::kAggregate) {
        ExprPtr holder = MakeLogical(LogicalOp::kNot, {item.expr});
        CollectAggregates(holder, plan->pipeline.output, &specs);
        item.expr = holder->children[0];
      } else {
        CollectAggregates(item.expr, plan->pipeline.output, &specs);
      }
    }
    if (statement_.having != nullptr) {
      ExprPtr holder = MakeLogical(LogicalOp::kNot, {statement_.having});
      CollectAggregates(holder, plan->pipeline.output, &specs);
      having_ = holder->children[0];
    }
    for (OrderTerm& term : order_terms_) {
      if (term.expr->kind == ExprKind::kAggregate) {
        ExprPtr holder = MakeLogical(LogicalOp::kNot, {term.expr});
        CollectAggregates(holder, plan->pipeline.output, &specs);
        term.expr = holder->children[0];
      } else {
        CollectAggregates(term.expr, plan->pipeline.output, &specs);
      }
    }
    plan->aggregates = specs;
    plan->has_aggregate = !specs.empty() || !group_terms_.empty();
    if (!plan->has_aggregate) {
      plan->post_schema = plan->pipeline.output;
      return;
    }
    Schema post;
    for (size_t i = 0; i < group_terms_.size(); ++i) {
      ExprPtr key = CloneExpr(group_terms_[i]);
      BindExpr(*key, plan->pipeline.output, &slots_);
      plan->group_keys.push_back(key);
      ColumnSchema column;
      column.name = key->kind == ExprKind::kColumn ? key->column_name : "grp_" + std::to_string(i);
      column.type = key->type;
      column.scale = key->scale;
      post.Add(column);
    }
    for (const AggregateSpec& spec : specs) {
      ColumnSchema column;
      column.name = spec.name;
      column.type = spec.output_type;
      column.scale = spec.output_scale;
      post.Add(column);
    }
    plan->post_schema = post;
    for (size_t i = 0; i < group_terms_.size(); ++i) {
      if (plan->group_keys[i]->kind == ExprKind::kColumn) {
        continue;
      }
      ExprPtr replacement = MakeColumn("", post.at(i).name);
      for (SelectItem& item : items_) {
        ReplaceEqual(item.expr, group_terms_[i], replacement);
      }
      for (OrderTerm& term : order_terms_) {
        ReplaceEqual(term.expr, group_terms_[i], replacement);
      }
      if (having_ != nullptr) {
        ReplaceEqual(having_, group_terms_[i], replacement);
      }
    }
  }

  void ReplaceEqual(ExprPtr& expr, const ExprPtr& target, const ExprPtr& replacement) {
    if (ExprEqual(*expr, *target)) {
      expr = CloneExpr(replacement);
      return;
    }
    for (ExprPtr& child : expr->children) {
      ReplaceEqual(child, target, replacement);
    }
  }

  void BuildProjections(QueryPlan* plan) {
    const Schema& input = plan->post_schema;
    for (size_t i = 0; i < items_.size(); ++i) {
      ProjectionItem projection;
      projection.expr = CloneExpr(items_[i].expr);
      BindExpr(*projection.expr, input, &slots_);
      projection.name = items_[i].alias;
      if (projection.name.empty()) {
        projection.name = items_[i].expr->kind == ExprKind::kColumn ? items_[i].expr->column_name
                                                                    : "column_" + std::to_string(i);
      }
      plan->projections.push_back(std::move(projection));
    }
    if (having_ != nullptr) {
      plan->having = CloneExpr(having_);
      BindExpr(*plan->having, input, &slots_);
    }
    for (const OrderTerm& term : order_terms_) {
      int matched = -1;
      if (term.expr->kind == ExprKind::kColumn) {
        for (size_t i = 0; i < items_.size(); ++i) {
          const std::string& alias = plan->projections[i].name;
          if (alias == term.expr->column_name) {
            matched = static_cast<int>(i);
            break;
          }
        }
      }
      if (matched < 0) {
        for (size_t i = 0; i < items_.size(); ++i) {
          if (ExprEqual(*items_[i].expr, *term.expr)) {
            matched = static_cast<int>(i);
            break;
          }
        }
      }
      OrderItem item;
      item.descending = term.descending;
      if (matched >= 0) {
        item.column = static_cast<size_t>(matched);
      } else {
        ProjectionItem hidden;
        hidden.expr = CloneExpr(term.expr);
        BindExpr(*hidden.expr, input, &slots_);
        hidden.name = "order_" + std::to_string(plan->projections.size());
        hidden.hidden = true;
        item.column = plan->projections.size();
        plan->projections.push_back(std::move(hidden));
      }
      plan->order.push_back(item);
    }
    for (const ProjectionItem& projection : plan->projections) {
      if (projection.hidden) {
        continue;
      }
      ColumnSchema column;
      column.name = projection.name;
      column.type = projection.expr->type;
      column.scale = projection.expr->scale;
      plan->result_schema.Add(column);
    }
  }

  const SelectStatement& statement_;
  Catalog* catalog_;
  const Config& config_;
  std::vector<TableEntry> tables_;
  std::vector<SelectItem> items_;
  std::map<std::string, size_t> column_owner_;
  std::set<std::string> aliases_;
  std::map<size_t, std::vector<ExprPtr>> pushed_;
  std::vector<Conjunct> edges_;
  std::vector<Conjunct> residuals_;
  ExprPtr having_;
  std::vector<OrderTerm> order_terms_;
  std::vector<ExprPtr> group_terms_;
  int slots_ = 0;
};

void DescribePipeline(const PipelinePlan& pipeline, int depth, std::string* out) {
  const std::string pad(depth * 2, ' ');
  *out += pad + "scan " + pipeline.scan.table + " [" + std::to_string(pipeline.scan.output.size()) +
          " of " + std::to_string(pipeline.scan.source_columns.size()) + " columns]\n";
  if (pipeline.scan.filter != nullptr) {
    *out += pad + "  filter " + DescribeExpr(*pipeline.scan.filter, pipeline.scan.output) + "\n";
  }
  for (const JoinSpec& join : pipeline.joins) {
    *out += pad + "hash join on ";
    for (size_t i = 0; i < join.probe_keys.size(); ++i) {
      if (i > 0) {
        *out += " and ";
      }
      *out += DescribeExpr(*join.probe_keys[i], join.output) + " = " +
              DescribeExpr(*join.build_keys[i], join.build->output);
    }
    *out += "\n";
    *out += pad + "  build side (" + std::to_string(join.build_rows) + " rows)\n";
    DescribePipeline(*join.build, depth + 2, out);
    if (join.residual != nullptr) {
      *out += pad + "  filter " + DescribeExpr(*join.residual, join.output) + "\n";
    }
  }
}

}

std::string Catalog::PathFor(const std::string& table) const {
  return (std::filesystem::path(directory_) / (table + ".vcq")).string();
}

bool Catalog::Exists(const std::string& table) const {
  return std::filesystem::exists(PathFor(table));
}

const TableReader& Catalog::Open(const std::string& table) {
  for (const auto& entry : open_) {
    if (entry.first == table) {
      return *entry.second;
    }
  }
  Check(Exists(table), "unknown table: " + table);
  open_.emplace_back(table, std::make_shared<TableReader>(PathFor(table)));
  return *open_.back().second;
}

std::vector<std::string> Catalog::Tables() const {
  std::vector<std::string> names;
  if (!std::filesystem::exists(directory_)) {
    return names;
  }
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.path().extension() == ".vcq") {
      names.push_back(entry.path().stem().string());
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

QueryPlan BuildPlan(const SelectStatement& statement, Catalog* catalog, const Config& config) {
  Planner planner(statement, catalog, config);
  return planner.Build();
}

std::string DescribePlan(const QueryPlan& plan) {
  std::string text;
  DescribePipeline(plan.pipeline, 0, &text);
  if (plan.has_aggregate) {
    text += "aggregate " + std::to_string(plan.aggregates.size()) + " over " +
            std::to_string(plan.group_keys.size()) + " group columns\n";
  }
  if (!plan.order.empty()) {
    text += "sort " + std::to_string(plan.order.size()) + " keys\n";
  }
  if (plan.limit >= 0) {
    text += "limit " + std::to_string(plan.limit) + "\n";
  }
  return text;
}

}
