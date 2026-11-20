#include "engine.h"

#include "error.h"
#include "kernels.h"
#include "parser.h"
#include "util.h"

namespace vcqe {

Engine::Engine(Config config, const std::string& directory)
    : config_(std::move(config)), catalog_(directory) {
  SetSimdEnabled(config_.execution.enable_simd);
  pool_ = std::make_unique<ThreadPool>(config_.ResolvedThreads());
}

void Engine::set_threads(uint32_t threads) {
  config_.execution.threads = threads;
  pool_ = std::make_unique<ThreadPool>(config_.ResolvedThreads());
}

LoadReport Engine::Load(const std::string& table, const std::string& source, const Schema& schema) {
  return LoadDelimited(source, catalog_.PathFor(table), schema, config_);
}

QueryRun Engine::Query(const std::string& sql) {
  const SelectStatement statement = ParseSelect(sql);
  const QueryPlan plan = BuildPlan(statement, &catalog_, config_);
  QueryRun run;
  Timer timer;
  if (config_.execution.row_mode()) {
    run.result = RunRowMode(plan, &catalog_, config_);
  } else {
    Executor executor(config_, pool_.get(), &catalog_);
    run.result = executor.Run(plan);
  }
  run.seconds = timer.Seconds();
  return run;
}

std::string Engine::Explain(const std::string& sql) {
  const SelectStatement statement = ParseSelect(sql);
  const QueryPlan plan = BuildPlan(statement, &catalog_, config_);
  return DescribePlan(plan);
}

}
