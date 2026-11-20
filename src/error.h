#pragma once

#include <stdexcept>
#include <string>

namespace vcqe {

class EngineError : public std::runtime_error {
public:
  explicit EngineError(const std::string& what) : std::runtime_error(what) {}
};

[[noreturn]] inline void Fail(const std::string& what) {
  throw EngineError(what);
}

inline void Check(bool condition, const std::string& what) {
  if (!condition) {
    throw EngineError(what);
  }
}

}
