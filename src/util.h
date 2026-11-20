#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace vcqe {

uint32_t Crc32c(const void* data, size_t size, uint32_t seed = 0);

uint32_t BitWidth(uint64_t range);
void BitPack(const uint64_t* values, size_t count, uint32_t width, std::vector<uint8_t>* out);
void BitUnpack(const uint8_t* data, size_t size, size_t count, uint32_t width, uint64_t* out);

class FileWriter {
public:
  explicit FileWriter(const std::string& path);
  void Append(const void* data, size_t size);
  uint64_t offset() const {
    return offset_;
  }
  void Close();

private:
  std::ofstream stream_;
  std::string path_;
  uint64_t offset_ = 0;
};

class FileReader {
public:
  explicit FileReader(const std::string& path);
  void ReadAt(uint64_t offset, void* out, size_t size);
  std::vector<uint8_t> ReadAt(uint64_t offset, size_t size);
  uint64_t size() const {
    return size_;
  }
  const std::string& path() const {
    return path_;
  }

private:
  std::ifstream stream_;
  std::string path_;
  uint64_t size_ = 0;
};

class Timer {
public:
  Timer() : start_(std::chrono::steady_clock::now()) {}
  void Reset() {
    start_ = std::chrono::steady_clock::now();
  }
  double Seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }

private:
  std::chrono::steady_clock::time_point start_;
};

std::string Trim(const std::string& text);
std::string ToLower(const std::string& text);
std::vector<std::string> Split(const std::string& text, char delimiter);
bool StartsWith(const std::string& text, const std::string& prefix);
bool EndsWith(const std::string& text, const std::string& suffix);
std::string ReadTextFile(const std::string& path);
bool LikeMatch(const char* text, size_t text_size, const std::string& pattern);

}
