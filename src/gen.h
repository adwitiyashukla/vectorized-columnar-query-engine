#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace vcqe {

std::vector<std::string> TpchTables();
Schema TpchSchema(const std::string& table);

struct GenerateReport {
  std::vector<std::pair<std::string, uint64_t>> tables;
  double seconds = 0.0;
};

GenerateReport GenerateTpch(const std::string& directory, double scale);

}
