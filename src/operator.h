#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "config.h"
#include "plan.h"
#include "scheduler.h"

namespace vcqe {

struct ExecStats {
  uint64_t row_groups_total = 0;
  uint64_t row_groups_skipped = 0;
  uint64_t rows_scanned = 0;
  uint64_t rows_after_filter = 0;
  uint64_t join_rows = 0;
  uint64_t bytes_read = 0;
};

struct QueryResult {
  Schema schema;
  std::vector<std::vector<Value>> rows;
  ExecStats stats;
};

class Sink {
public:
  virtual ~Sink() = default;
  virtual void Consume(Batch& batch) = 0;
};

class Executor {
public:
  Executor(const Config& config, ThreadPool* pool, Catalog* catalog);
  QueryResult Run(const QueryPlan& plan);

  void RunPipeline(const PipelinePlan& pipeline, std::vector<Sink*> sinks, ExecStats* stats);

private:
  std::shared_ptr<JoinHashTable> BuildJoin(const JoinSpec& join, ExecStats* stats);

  const Config& config_;
  ThreadPool* pool_;
  Catalog* catalog_;
};

bool ZoneMapSkips(const Expr& predicate, const std::vector<ChunkMeta>& chunks,
                  const std::vector<size_t>& mapping);

}
