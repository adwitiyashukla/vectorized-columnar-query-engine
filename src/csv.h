#pragma once

#include <string>

#include "config.h"
#include "types.h"

namespace vcqe {

struct LoadReport {
  uint64_t rows = 0;
  uint64_t source_bytes = 0;
  uint64_t target_bytes = 0;
  double seconds = 0.0;
};

LoadReport LoadDelimited(const std::string& source, const std::string& target, const Schema& schema,
                         const Config& config);

Schema ParseSchemaText(const std::string& text);

}
