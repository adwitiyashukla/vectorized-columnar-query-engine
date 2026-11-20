#pragma once

#include <memory>
#include <string>

#include "config.h"
#include "csv.h"
#include "operator.h"
#include "plan.h"
#include "scheduler.h"

namespace vcqe {

struct QueryRun {
  QueryResult result;
  double seconds = 0.0;
  std::string plan_text;
};

class Engine {
public:
  Engine(Config config, const std::string& directory);

  LoadReport Load(const std::string& table, const std::string& source, const Schema& schema);
  QueryRun Query(const std::string& sql);
  std::string Explain(const std::string& sql);
  Catalog& catalog() {
    return catalog_;
  }
  const Config& config() const {
    return config_;
  }
  void set_mode(const std::string& mode) {
    config_.execution.mode = mode;
  }
  void set_threads(uint32_t threads);

private:
  Config config_;
  Catalog catalog_;
  std::unique_ptr<ThreadPool> pool_;
};

QueryResult RunRowMode(const QueryPlan& plan, Catalog* catalog, const Config& config);

}
