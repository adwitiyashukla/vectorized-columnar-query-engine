#pragma once

#include <filesystem>
#include <string>

#include "config.h"
#include "csv.h"
#include "gen.h"

namespace vcqe {

inline std::string SmallDataset() {
  static std::string directory;
  if (!directory.empty()) {
    return directory;
  }
  directory = (std::filesystem::temp_directory_path() / "vcqe_test_data").string();
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  GenerateTpch(directory, 0.005);
  Config config;
  config.storage.row_group_rows = 4096;
  for (const std::string& table : TpchTables()) {
    LoadDelimited((std::filesystem::path(directory) / (table + ".tbl")).string(),
                  (std::filesystem::path(directory) / (table + ".vcq")).string(), TpchSchema(table),
                  config);
  }
  return directory;
}

}
