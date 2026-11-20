#include "operator.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "error.h"
#include "kernels.h"
#include "util.h"

namespace vcqe {
namespace {

void GatherInto(const Vector& source, const uint32_t* rows, size_t count, Vector& target) {
  target.ResetValidity();
  switch (source.type()) {
  case DataType::kInt32:
  case DataType::kDate: {
    const int32_t* from = source.i32();
    int32_t* to = target.i32();
    for (size_t i = 0; i < count; ++i) {
      to[i] = from[rows[i]];
    }
    break;
  }
  case DataType::kInt64:
  case DataType::kDecimal: {
    const int64_t* from = source.i64();
    int64_t* to = target.i64();
    for (size_t i = 0; i < count; ++i) {
      to[i] = from[rows[i]];
    }
    break;
  }
  case DataType::kDouble: {
    const double* from = source.f64();
    double* to = target.f64();
    for (size_t i = 0; i < count; ++i) {
      to[i] = from[rows[i]];
    }
    break;
  }
  case DataType::kString: {
    const StringRef* from = source.strings();
    target.PrepareStrings(count);
    for (size_t i = 0; i < count; ++i) {
      const StringRef& text = from[rows[i]];
      target.AppendString(i, text.data, text.size);
    }
    target.FinishStrings(count);
    break;
  }
  }
  if (!source.validity().all_valid()) {
    for (size_t i = 0; i < count; ++i) {
      if (!source.validity().Get(rows[i])) {
        target.validity().SetNull(i);
      }
    }
  }
}

bool ZoneSkipsCompare(const Expr& expr, const std::vector<ChunkMeta>& chunks,
                      const std::vector<size_t>& mapping) {
  if (expr.children[0]->kind != ExprKind::kColumn ||
      expr.children[1]->kind != ExprKind::kConstant) {
    return false;
  }
  const size_t column = expr.children[0]->column;
  if (column >= mapping.size()) {
    return false;
  }
  const ChunkMeta& chunk = chunks[mapping[column]];
  if (!chunk.zone.valid || chunk.null_count > 0) {
    return false;
  }
  const Value& constant = expr.children[1]->constant;
  if (constant.is_null) {
    return false;
  }
  if (constant.type == DataType::kString) {
    if (chunk.zone.min_text.empty() && chunk.zone.max_text.empty()) {
      return false;
    }
    const std::string& value = constant.text;
    switch (expr.compare) {
    case CompareOp::kEqual:
      return value < chunk.zone.min_text || value > chunk.zone.max_text;
    case CompareOp::kLess:
      return value <= chunk.zone.min_text;
    case CompareOp::kLessEqual:
      return value < chunk.zone.min_text;
    case CompareOp::kGreater:
      return value >= chunk.zone.max_text;
    case CompareOp::kGreaterEqual:
      return value > chunk.zone.max_text;
    default:
      return false;
    }
  }
  if (constant.type == DataType::kDouble) {
    const double value = constant.real;
    switch (expr.compare) {
    case CompareOp::kEqual:
      return value < chunk.zone.min_real || value > chunk.zone.max_real;
    case CompareOp::kLess:
      return value <= chunk.zone.min_real;
    case CompareOp::kLessEqual:
      return value < chunk.zone.min_real;
    case CompareOp::kGreater:
      return value >= chunk.zone.max_real;
    case CompareOp::kGreaterEqual:
      return value > chunk.zone.max_real;
    default:
      return false;
    }
  }
  const int64_t value = constant.integer;
  switch (expr.compare) {
  case CompareOp::kEqual:
    return value < chunk.zone.min_integer || value > chunk.zone.max_integer;
  case CompareOp::kLess:
    return value <= chunk.zone.min_integer;
  case CompareOp::kLessEqual:
    return value < chunk.zone.min_integer;
  case CompareOp::kGreater:
    return value >= chunk.zone.max_integer;
  case CompareOp::kGreaterEqual:
    return value > chunk.zone.max_integer;
  default:
    return false;
  }
}

class ScanSource {
public:
  ScanSource(const ScanSpec& scan, const TableReader& table, const Config& config)
      : scan_(scan), table_(table), cursor_(table), config_(config) {
    chunks_.resize(scan_.source_columns.size());
    batch_.Init(scan_.output.columns(), config_.execution.vector_size);
  }

  bool LoadGroup(size_t group, ExecStats* stats) {
    const std::vector<ChunkMeta>& chunks = table_.meta().row_groups[group].chunks;
    if (config_.execution.enable_zone_maps && scan_.filter != nullptr &&
        ZoneMapSkips(*scan_.filter, chunks, scan_.source_columns)) {
      ++stats->row_groups_skipped;
      return false;
    }
    for (size_t i = 0; i < scan_.source_columns.size(); ++i) {
      cursor_.Read(group, scan_.source_columns[i], &chunks_[i]);
      stats->bytes_read += chunks[scan_.source_columns[i]].size;
    }
    rows_ = static_cast<size_t>(table_.RowGroupRows(group));
    position_ = 0;
    stats->rows_scanned += rows_;
    return true;
  }

  Batch* NextBatch() {
    if (position_ >= rows_) {
      return nullptr;
    }
    const size_t count = std::min<size_t>(rows_ - position_, config_.execution.vector_size);
    for (size_t i = 0; i < chunks_.size(); ++i) {
      Vector& target = batch_.column(i);
      const ColumnChunk& chunk = chunks_[i];
      target.ResetValidity();
      if (chunk.type == DataType::kString) {
        StringRef* to = target.strings();
        std::memcpy(to, chunk.strings.data() + position_, count * sizeof(StringRef));
      } else {
        const size_t width = TypeWidth(chunk.type);
        std::memcpy(target.bytes(), chunk.fixed.data() + position_ * width, count * width);
      }
      if (!chunk.all_valid) {
        for (size_t row = 0; row < count; ++row) {
          if (!chunk.IsValid(position_ + row)) {
            target.validity().SetNull(row);
          }
        }
      }
    }
    position_ += count;
    batch_.set_count(count);
    batch_.set_has_selection(false);
    return &batch_;
  }

private:
  const ScanSpec& scan_;
  const TableReader& table_;
  ChunkCursor cursor_;
  const Config& config_;
  std::vector<ColumnChunk> chunks_;
  Batch batch_;
  size_t rows_ = 0;
  size_t position_ = 0;
};

struct JoinStage {
  Batch output;
  std::vector<uint64_t> hashes;
  std::vector<uint32_t> probe_rows;
  std::vector<const uint8_t*> build_rows;
  ProbeState probe;
};

class PipelineRunner {
public:
  PipelineRunner(const PipelinePlan& plan, const Config& config, const TableReader& table,
                 const std::vector<std::shared_ptr<JoinHashTable>>& tables, Sink* sink, int slots)
      : plan_(plan), config_(config), source_(plan.scan, table, config), tables_(tables),
        sink_(sink) {
    context_.Init(slots, config.execution.vector_size);
    stages_.resize(plan_.joins.size());
    for (size_t i = 0; i < plan_.joins.size(); ++i) {
      stages_[i].output.Init(plan_.joins[i].output.columns(), config.execution.vector_size);
      stages_[i].hashes.assign(config.execution.vector_size, 0);
      stages_[i].probe_rows.assign(config.execution.vector_size, 0);
      stages_[i].build_rows.assign(config.execution.vector_size, nullptr);
    }
  }

  void RunGroup(size_t group, ExecStats* stats) {
    if (!source_.LoadGroup(group, stats)) {
      return;
    }
    while (Batch* batch = source_.NextBatch()) {
      if (plan_.scan.filter != nullptr) {
        const size_t matched = FilterBatch(*plan_.scan.filter, *batch, nullptr, batch->count(),
                                           batch->selection(), &context_);
        batch->set_has_selection(true);
        batch->set_count(matched);
      }
      stats->rows_after_filter += batch->count();
      if (batch->count() == 0) {
        continue;
      }
      Push(0, *batch, stats);
    }
  }

private:
  void Push(size_t index, Batch& input, ExecStats* stats) {
    if (index == plan_.joins.size()) {
      sink_->Consume(input);
      return;
    }
    const JoinSpec& join = plan_.joins[index];
    JoinHashTable& table = *tables_[index];
    JoinStage& stage = stages_[index];
    if (table.empty()) {
      return;
    }
    std::vector<const Vector*> keys;
    keys.reserve(join.probe_keys.size());
    const uint32_t* sel = input.selection_or_null();
    const size_t count = input.count();
    for (const ExprPtr& key : join.probe_keys) {
      keys.push_back(&EvalValue(*key, input, sel, count, &context_));
    }
    for (size_t i = 0; i < keys.size(); ++i) {
      HashVector(*keys[i], sel, count, stage.hashes.data(), i > 0);
    }
    stage.probe.Reset();
    const size_t width = input.width();
    while (true) {
      const size_t produced =
          table.Probe(keys, sel, count, stage.hashes.data(), &stage.probe, stage.probe_rows.data(),
                      stage.build_rows.data(), stage.output.capacity());
      if (produced == 0) {
        break;
      }
      stats->join_rows += produced;
      for (size_t column = 0; column < width; ++column) {
        GatherInto(input.column(column), stage.probe_rows.data(), produced,
                   stage.output.column(column));
      }
      for (size_t p = 0; p < join.payload.size(); ++p) {
        Vector& target = stage.output.column(width + p);
        target.ResetValidity();
        if (target.type() == DataType::kString) {
          target.PrepareStrings(produced);
        }
        for (size_t row = 0; row < produced; ++row) {
          table.Gather(stage.build_rows[row], p, target, row);
        }
        if (target.type() == DataType::kString) {
          target.FinishStrings(produced);
        }
      }
      stage.output.set_count(produced);
      stage.output.set_has_selection(false);
      if (join.residual != nullptr) {
        const size_t matched = FilterBatch(*join.residual, stage.output, nullptr, produced,
                                           stage.output.selection(), &context_);
        stage.output.set_has_selection(true);
        stage.output.set_count(matched);
        if (matched == 0) {
          continue;
        }
      }
      Push(index + 1, stage.output, stats);
    }
  }

  const PipelinePlan& plan_;
  const Config& config_;
  ScanSource source_;
  const std::vector<std::shared_ptr<JoinHashTable>>& tables_;
  Sink* sink_;
  ExprContext context_;
  std::vector<JoinStage> stages_;
};

class JoinBuildSink : public Sink {
public:
  JoinBuildSink(JoinHashTable* table, std::mutex* mutex, const JoinSpec& join, const Config& config,
                int slots)
      : table_(table), mutex_(mutex), join_(join) {
    context_.Init(slots, config.execution.vector_size);
    hashes_.assign(config.execution.vector_size, 0);
  }

  void Consume(Batch& batch) override {
    const uint32_t* sel = batch.selection_or_null();
    const size_t count = batch.count();
    if (count == 0) {
      return;
    }
    std::vector<const Vector*> keys;
    for (const ExprPtr& key : join_.build_keys) {
      keys.push_back(&EvalValue(*key, batch, sel, count, &context_));
    }
    std::vector<const Vector*> payload;
    for (size_t column : join_.payload) {
      payload.push_back(&batch.column(column));
    }
    std::lock_guard<std::mutex> guard(*mutex_);
    table_->Insert(keys, payload, sel, count, hashes_.data());
  }

private:
  JoinHashTable* table_;
  std::mutex* mutex_;
  const JoinSpec& join_;
  ExprContext context_;
  std::vector<uint64_t> hashes_;
};

class AggregateSink : public Sink {
public:
  AggregateSink(const QueryPlan& plan, const Config& config, int slots) : plan_(plan) {
    std::vector<DataType> key_types;
    for (const ExprPtr& key : plan.group_keys) {
      key_types.push_back(key->type);
    }
    table_.Init(std::move(key_types), plan.aggregates, config.execution.aggregate_initial_buckets,
                config.execution.hash_table_load_factor);
    context_.Init(slots, config.execution.vector_size);
    hashes_.assign(config.execution.vector_size, 0);
  }

  void Consume(Batch& batch) override {
    const uint32_t* sel = batch.selection_or_null();
    const size_t count = batch.count();
    if (count == 0) {
      return;
    }
    std::vector<const Vector*> keys;
    for (const ExprPtr& key : plan_.group_keys) {
      keys.push_back(&EvalValue(*key, batch, sel, count, &context_));
    }
    std::vector<const Vector*> inputs;
    for (const AggregateSpec& spec : plan_.aggregates) {
      if (spec.input == nullptr) {
        inputs.push_back(nullptr);
      } else {
        inputs.push_back(&EvalValue(*spec.input, batch, sel, count, &context_));
      }
    }
    table_.AddBatch(keys, inputs, sel, count, hashes_.data());
  }

  AggregateTable& table() {
    return table_;
  }

private:
  const QueryPlan& plan_;
  AggregateTable table_;
  ExprContext context_;
  std::vector<uint64_t> hashes_;
};

class CollectSink : public Sink {
public:
  CollectSink(const QueryPlan& plan, const Config& config, int slots) : plan_(plan) {
    context_.Init(slots, config.execution.vector_size);
  }

  void Consume(Batch& batch) override {
    const uint32_t* sel = batch.selection_or_null();
    const size_t count = batch.count();
    if (count == 0) {
      return;
    }
    std::vector<const Vector*> values;
    for (const ProjectionItem& item : plan_.projections) {
      values.push_back(&EvalValue(*item.expr, batch, sel, count, &context_));
    }
    for (size_t i = 0; i < count; ++i) {
      const size_t row = sel == nullptr ? i : sel[i];
      std::vector<Value> output;
      output.reserve(values.size());
      for (const Vector* value : values) {
        output.push_back(value->GetValue(row));
      }
      rows_.push_back(std::move(output));
    }
    Check(rows_.size() <= 20000000, "query would return too many rows");
  }

  std::vector<std::vector<Value>>& rows() {
    return rows_;
  }

private:
  const QueryPlan& plan_;
  ExprContext context_;
  std::vector<std::vector<Value>> rows_;
};

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

bool ZoneMapSkips(const Expr& predicate, const std::vector<ChunkMeta>& chunks,
                  const std::vector<size_t>& mapping) {
  switch (predicate.kind) {
  case ExprKind::kLogical:
    if (predicate.logical == LogicalOp::kAnd) {
      for (const ExprPtr& child : predicate.children) {
        if (ZoneMapSkips(*child, chunks, mapping)) {
          return true;
        }
      }
      return false;
    }
    if (predicate.logical == LogicalOp::kOr) {
      for (const ExprPtr& child : predicate.children) {
        if (!ZoneMapSkips(*child, chunks, mapping)) {
          return false;
        }
      }
      return true;
    }
    return false;
  case ExprKind::kCompare:
    return ZoneSkipsCompare(predicate, chunks, mapping);
  default:
    return false;
  }
}

Executor::Executor(const Config& config, ThreadPool* pool, Catalog* catalog)
    : config_(config), pool_(pool), catalog_(catalog) {}

std::shared_ptr<JoinHashTable> Executor::BuildJoin(const JoinSpec& join, ExecStats* stats) {
  std::vector<DataType> key_types;
  for (const ExprPtr& key : join.build_keys) {
    key_types.push_back(key->type);
  }
  auto table = std::make_shared<JoinHashTable>();
  const size_t buckets = static_cast<size_t>(join.build_rows) + 16;
  table->Init(std::move(key_types), join.payload_schema, buckets,
              config_.execution.hash_table_load_factor);
  std::mutex mutex;
  std::vector<std::unique_ptr<JoinBuildSink>> sinks;
  std::vector<Sink*> pointers;
  for (size_t i = 0; i < pool_->size(); ++i) {
    sinks.push_back(
        std::make_unique<JoinBuildSink>(table.get(), &mutex, join, config_, join.build->slots));
    pointers.push_back(sinks.back().get());
  }
  RunPipeline(*join.build, pointers, stats);
  return table;
}

void Executor::RunPipeline(const PipelinePlan& pipeline, std::vector<Sink*> sinks,
                           ExecStats* stats) {
  std::vector<std::shared_ptr<JoinHashTable>> tables;
  for (const JoinSpec& join : pipeline.joins) {
    tables.push_back(BuildJoin(join, stats));
  }
  const TableReader& table = catalog_->Open(pipeline.scan.table);
  const size_t groups = table.row_group_count();
  stats->row_groups_total += groups;
  MorselQueue queue(groups);
  std::vector<ExecStats> local(sinks.size());
  pool_->Run(sinks.size(), [&](size_t worker) {
    PipelineRunner runner(pipeline, config_, table, tables, sinks[worker], pipeline.slots);
    size_t group = 0;
    while (queue.Next(&group)) {
      runner.RunGroup(group, &local[worker]);
    }
  });
  for (const ExecStats& partial : local) {
    stats->row_groups_skipped += partial.row_groups_skipped;
    stats->rows_scanned += partial.rows_scanned;
    stats->rows_after_filter += partial.rows_after_filter;
    stats->join_rows += partial.join_rows;
    stats->bytes_read += partial.bytes_read;
  }
}

QueryResult Executor::Run(const QueryPlan& plan) {
  QueryResult result;
  result.schema = plan.result_schema;
  std::vector<std::vector<Value>> rows;

  if (plan.has_aggregate) {
    std::vector<std::unique_ptr<AggregateSink>> sinks;
    std::vector<Sink*> pointers;
    for (size_t i = 0; i < pool_->size(); ++i) {
      sinks.push_back(std::make_unique<AggregateSink>(plan, config_, plan.slots));
      pointers.push_back(sinks.back().get());
    }
    RunPipeline(plan.pipeline, pointers, &result.stats);
    AggregateTable& merged = sinks[0]->table();
    for (size_t i = 1; i < sinks.size(); ++i) {
      merged.MergeFrom(sinks[i]->table());
    }
    const size_t keys = plan.group_keys.size();
    std::vector<Value> post(keys + plan.aggregates.size());
    for (size_t group = 0; group < merged.group_count(); ++group) {
      for (size_t k = 0; k < keys; ++k) {
        post[k] = merged.GroupKey(group, k);
        post[k].scale = plan.group_keys[k]->scale;
      }
      for (size_t a = 0; a < plan.aggregates.size(); ++a) {
        post[keys + a] = merged.Result(group, a);
      }
      if (plan.having != nullptr && !EvalRowBool(*plan.having, post)) {
        continue;
      }
      std::vector<Value> output;
      output.reserve(plan.projections.size());
      for (const ProjectionItem& item : plan.projections) {
        output.push_back(EvalRowValue(*item.expr, post));
      }
      rows.push_back(std::move(output));
    }
    if (merged.group_count() == 0 && plan.group_keys.empty()) {
      std::vector<Value> empty(keys + plan.aggregates.size());
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
    std::vector<std::unique_ptr<CollectSink>> sinks;
    std::vector<Sink*> pointers;
    for (size_t i = 0; i < pool_->size(); ++i) {
      sinks.push_back(std::make_unique<CollectSink>(plan, config_, plan.slots));
      pointers.push_back(sinks.back().get());
    }
    RunPipeline(plan.pipeline, pointers, &result.stats);
    for (auto& sink : sinks) {
      for (std::vector<Value>& row : sink->rows()) {
        rows.push_back(std::move(row));
      }
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
  for (std::vector<Value>& row : rows) {
    row.resize(visible);
    result.rows.push_back(std::move(row));
  }
  return result;
}

}
