#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "error.h"
#include "hash_table.h"
#include "reader.h"

namespace vcqe {
namespace {

Value ChunkValue(const ColumnChunk& chunk, size_t index) {
  if (!chunk.IsValid(index)) {
    return Value::Null(chunk.type);
  }
  switch (chunk.type) {
  case DataType::kInt32:
    return Value::Int(chunk.i32()[index], DataType::kInt32);
  case DataType::kDate:
    return Value::Date(chunk.i32()[index]);
  case DataType::kInt64:
    return Value::Int(chunk.i64()[index], DataType::kInt64);
  case DataType::kDecimal:
    return Value::Decimal(chunk.i64()[index], chunk.scale);
  case DataType::kDouble:
    return Value::Real(chunk.f64()[index]);
  case DataType::kString:
    return Value::String(chunk.strings[index].ToString());
  }
  return Value::Null(chunk.type);
}

class RowIterator {
public:
  virtual ~RowIterator() = default;
  virtual bool Next(std::vector<Value>* row) = 0;
};

class RowScan : public RowIterator {
public:
  RowScan(const ScanSpec& scan, const TableReader& table, ExecStats* stats)
      : scan_(scan), table_(table), cursor_(table), stats_(stats) {
    chunks_.resize(scan_.source_columns.size());
    stats_->row_groups_total += table_.row_group_count();
  }

  bool Next(std::vector<Value>* row) override {
    while (true) {
      if (position_ >= rows_) {
        if (!LoadNextGroup()) {
          return false;
        }
        continue;
      }
      row->resize(chunks_.size());
      for (size_t i = 0; i < chunks_.size(); ++i) {
        (*row)[i] = ChunkValue(chunks_[i], position_);
      }
      ++position_;
      ++stats_->rows_scanned;
      if (scan_.filter != nullptr && !EvalRowBool(*scan_.filter, *row)) {
        continue;
      }
      ++stats_->rows_after_filter;
      return true;
    }
  }

private:
  bool LoadNextGroup() {
    while (group_ < table_.row_group_count()) {
      const size_t group = group_++;
      for (size_t i = 0; i < scan_.source_columns.size(); ++i) {
        cursor_.Read(group, scan_.source_columns[i], &chunks_[i]);
      }
      rows_ = static_cast<size_t>(table_.RowGroupRows(group));
      position_ = 0;
      return true;
    }
    return false;
  }

  const ScanSpec& scan_;
  const TableReader& table_;
  ChunkCursor cursor_;
  ExecStats* stats_;
  std::vector<ColumnChunk> chunks_;
  size_t group_ = 0;
  size_t rows_ = 0;
  size_t position_ = 0;
};

class RowJoin : public RowIterator {
public:
  RowJoin(std::unique_ptr<RowIterator> probe, std::unique_ptr<RowIterator> build,
          const JoinSpec& join, ExecStats* stats)
      : probe_(std::move(probe)), build_(std::move(build)), join_(join), stats_(stats) {}

  bool Next(std::vector<Value>* row) override {
    if (!built_) {
      Build();
    }
    while (true) {
      if (match_ < matches_.size()) {
        const std::vector<Value>& build_row = rows_[matches_[match_++]];
        row->assign(current_.begin(), current_.end());
        for (size_t column : join_.payload) {
          row->push_back(build_row[column]);
        }
        ++stats_->join_rows;
        if (join_.residual != nullptr && !EvalRowBool(*join_.residual, *row)) {
          continue;
        }
        return true;
      }
      if (!probe_->Next(&current_)) {
        return false;
      }
      std::vector<Value> keys;
      keys.reserve(join_.probe_keys.size());
      for (const ExprPtr& key : join_.probe_keys) {
        keys.push_back(EvalRowValue(*key, current_));
      }
      matches_.clear();
      match_ = 0;
      const auto found = index_.find(HashValues(keys));
      if (found == index_.end()) {
        continue;
      }
      for (size_t candidate : found->second) {
        if (KeysMatch(rows_[candidate], keys)) {
          matches_.push_back(candidate);
        }
      }
    }
  }

private:
  bool KeysMatch(const std::vector<Value>& build_row, const std::vector<Value>& keys) {
    for (size_t i = 0; i < keys.size(); ++i) {
      const Value stored = EvalRowValue(*join_.build_keys[i], build_row);
      if (stored.is_null || keys[i].is_null) {
        return false;
      }
      if (stored.type == DataType::kString) {
        if (stored.text != keys[i].text) {
          return false;
        }
      } else if (stored.type == DataType::kDouble) {
        if (stored.real != keys[i].real) {
          return false;
        }
      } else if (stored.integer != keys[i].integer) {
        return false;
      }
    }
    return true;
  }

  void Build() {
    built_ = true;
    std::vector<Value> row;
    while (build_->Next(&row)) {
      std::vector<Value> keys;
      keys.reserve(join_.build_keys.size());
      for (const ExprPtr& key : join_.build_keys) {
        keys.push_back(EvalRowValue(*key, row));
      }
      index_[HashValues(keys)].push_back(rows_.size());
      rows_.push_back(row);
    }
  }

  std::unique_ptr<RowIterator> probe_;
  std::unique_ptr<RowIterator> build_;
  const JoinSpec& join_;
  ExecStats* stats_;
  std::unordered_map<uint64_t, std::vector<size_t>> index_;
  std::vector<std::vector<Value>> rows_;
  std::vector<Value> current_;
  std::vector<size_t> matches_;
  size_t match_ = 0;
  bool built_ = false;
};

std::unique_ptr<RowIterator> BuildIterator(const PipelinePlan& pipeline, Catalog* catalog,
                                           ExecStats* stats) {
  std::unique_ptr<RowIterator> current =
      std::make_unique<RowScan>(pipeline.scan, catalog->Open(pipeline.scan.table), stats);
  for (const JoinSpec& join : pipeline.joins) {
    std::unique_ptr<RowIterator> build = BuildIterator(*join.build, catalog, stats);
    current = std::make_unique<RowJoin>(std::move(current), std::move(build), join, stats);
  }
  return current;
}

int CompareValues(const Value& left, const Value& right) {
  if (left.is_null || right.is_null) {
    if (left.is_null && right.is_null) {
      return 0;
    }
    return left.is_null ? -1 : 1;
  }
  if (left.type == DataType::kString) {
    const int order = left.text.compare(right.text);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
  }
  if (left.type == DataType::kDouble) {
    return left.real < right.real ? -1 : (left.real > right.real ? 1 : 0);
  }
  return left.integer < right.integer ? -1 : (left.integer > right.integer ? 1 : 0);
}

}

QueryResult RunRowMode(const QueryPlan& plan, Catalog* catalog, const Config& config) {
  QueryResult result;
  result.schema = plan.result_schema;
  std::unique_ptr<RowIterator> iterator = BuildIterator(plan.pipeline, catalog, &result.stats);
  std::vector<std::vector<Value>> rows;
  std::vector<Value> row;

  if (plan.has_aggregate) {
    std::vector<DataType> key_types;
    for (const ExprPtr& key : plan.group_keys) {
      key_types.push_back(key->type);
    }
    AggregateTable table;
    table.Init(key_types, plan.aggregates, config.execution.aggregate_initial_buckets,
               config.execution.hash_table_load_factor);
    std::vector<Value> keys(plan.group_keys.size());
    std::vector<Value> inputs(plan.aggregates.size());
    while (iterator->Next(&row)) {
      for (size_t i = 0; i < plan.group_keys.size(); ++i) {
        keys[i] = EvalRowValue(*plan.group_keys[i], row);
      }
      for (size_t i = 0; i < plan.aggregates.size(); ++i) {
        inputs[i] = plan.aggregates[i].input == nullptr
                        ? Value::Int(0, DataType::kInt64)
                        : EvalRowValue(*plan.aggregates[i].input, row);
      }
      table.AddRow(keys, inputs);
    }
    std::vector<Value> post(plan.group_keys.size() + plan.aggregates.size());
    for (size_t group = 0; group < table.group_count(); ++group) {
      for (size_t k = 0; k < plan.group_keys.size(); ++k) {
        post[k] = table.GroupKey(group, k);
        post[k].scale = plan.group_keys[k]->scale;
      }
      for (size_t a = 0; a < plan.aggregates.size(); ++a) {
        post[plan.group_keys.size() + a] = table.Result(group, a);
      }
      if (plan.having != nullptr && !EvalRowBool(*plan.having, post)) {
        continue;
      }
      std::vector<Value> output;
      for (const ProjectionItem& item : plan.projections) {
        output.push_back(EvalRowValue(*item.expr, post));
      }
      rows.push_back(std::move(output));
    }
    if (table.group_count() == 0 && plan.group_keys.empty()) {
      std::vector<Value> empty(plan.aggregates.size());
      for (size_t a = 0; a < plan.aggregates.size(); ++a) {
        const AggregateSpec& spec = plan.aggregates[a];
        empty[a] = spec.kind == AggregateKind::kCountStar || spec.kind == AggregateKind::kCount
                       ? Value::Int(0, DataType::kInt64)
                       : Value::Null(spec.output_type);
      }
      std::vector<Value> output;
      for (const ProjectionItem& item : plan.projections) {
        output.push_back(EvalRowValue(*item.expr, empty));
      }
      rows.push_back(std::move(output));
    }
  } else {
    while (iterator->Next(&row)) {
      std::vector<Value> output;
      output.reserve(plan.projections.size());
      for (const ProjectionItem& item : plan.projections) {
        output.push_back(EvalRowValue(*item.expr, row));
      }
      rows.push_back(std::move(output));
      Check(rows.size() <= 20000000, "query would return too many rows");
    }
  }

  if (!plan.order.empty()) {
    std::stable_sort(rows.begin(), rows.end(),
                     [&plan](const std::vector<Value>& left, const std::vector<Value>& right) {
                       for (const OrderItem& item : plan.order) {
                         const int order = CompareValues(left[item.column], right[item.column]);
                         if (order != 0) {
                           return item.descending ? order > 0 : order < 0;
                         }
                       }
                       return false;
                     });
  }
  if (plan.limit >= 0 && rows.size() > static_cast<size_t>(plan.limit)) {
    rows.resize(static_cast<size_t>(plan.limit));
  }
  size_t visible = 0;
  for (const ProjectionItem& item : plan.projections) {
    if (!item.hidden) {
      ++visible;
    }
  }
  for (std::vector<Value>& output : rows) {
    output.resize(visible);
    result.rows.push_back(std::move(output));
  }
  return result;
}

}
