#include "util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <sstream>

#include "error.h"

namespace vcqe {
namespace {

std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) ? (value >> 1) ^ 0x82F63B78u : (value >> 1);
    }
    table[i] = value;
  }
  return table;
}

const std::array<uint32_t, 256>& CrcTable() {
  static const std::array<uint32_t, 256> table = MakeCrcTable();
  return table;
}

}

uint32_t Crc32c(const void* data, size_t size, uint32_t seed) {
  const auto& table = CrcTable();
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = ~seed;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

uint32_t BitWidth(uint64_t range) {
  uint32_t width = 0;
  while (range > 0) {
    ++width;
    range >>= 1;
  }
  return width;
}

void BitPack(const uint64_t* values, size_t count, uint32_t width, std::vector<uint8_t>* out) {
  out->clear();
  if (width == 0 || count == 0) {
    return;
  }
  out->reserve((count * width + 7) / 8);
  uint64_t buffer = 0;
  uint32_t bits = 0;
  for (size_t i = 0; i < count; ++i) {
    uint64_t value = width == 64 ? values[i] : (values[i] & ((1ull << width) - 1));
    uint32_t remaining = width;
    while (remaining > 0) {
      const uint32_t free_bits = 64 - bits;
      const uint32_t take = remaining < free_bits ? remaining : free_bits;
      const uint64_t part = take == 64 ? value : (value & ((1ull << take) - 1));
      buffer |= part << bits;
      value = take == 64 ? 0 : (value >> take);
      bits += take;
      remaining -= take;
      while (bits >= 8) {
        out->push_back(static_cast<uint8_t>(buffer & 0xFFu));
        buffer >>= 8;
        bits -= 8;
      }
    }
  }
  if (bits > 0) {
    out->push_back(static_cast<uint8_t>(buffer & 0xFFu));
  }
}

void BitUnpack(const uint8_t* data, size_t size, size_t count, uint32_t width, uint64_t* out) {
  if (width == 0) {
    for (size_t i = 0; i < count; ++i) {
      out[i] = 0;
    }
    return;
  }
  Check(size >= (count * width + 7) / 8, "bit packed block is too small");
  size_t bit_position = 0;
  const uint64_t mask = width == 64 ? ~0ull : ((1ull << width) - 1);
  for (size_t i = 0; i < count; ++i) {
    const size_t byte_index = bit_position >> 3;
    const uint32_t bit_offset = static_cast<uint32_t>(bit_position & 7);
    uint64_t window = 0;
    const size_t available = size - byte_index;
    const size_t take = available < 8 ? available : 8;
    std::memcpy(&window, data + byte_index, take);
    uint64_t value = window >> bit_offset;
    if (bit_offset + width > 64) {
      uint64_t extra = 0;
      const size_t next = byte_index + 8;
      if (next < size) {
        const size_t take_next = size - next < 8 ? size - next : 8;
        std::memcpy(&extra, data + next, take_next);
      }
      value |= extra << (64 - bit_offset);
    }
    out[i] = value & mask;
    bit_position += width;
  }
}

FileWriter::FileWriter(const std::string& path) : path_(path) {
  stream_.open(path, std::ios::binary | std::ios::trunc);
  Check(stream_.good(), "cannot open file for writing: " + path);
}

void FileWriter::Append(const void* data, size_t size) {
  if (size == 0) {
    return;
  }
  stream_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  Check(stream_.good(), "write failed: " + path_);
  offset_ += size;
}

void FileWriter::Close() {
  if (stream_.is_open()) {
    stream_.flush();
    Check(stream_.good(), "flush failed: " + path_);
    stream_.close();
  }
}

FileReader::FileReader(const std::string& path) : path_(path) {
  stream_.open(path, std::ios::binary);
  Check(stream_.good(), "cannot open file for reading: " + path);
  stream_.seekg(0, std::ios::end);
  size_ = static_cast<uint64_t>(stream_.tellg());
}

void FileReader::ReadAt(uint64_t offset, void* out, size_t size) {
  Check(offset + size <= size_, "read past end of file: " + path_);
  stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  stream_.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
  Check(stream_.good() || stream_.gcount() == static_cast<std::streamsize>(size),
        "read failed: " + path_);
}

std::vector<uint8_t> FileReader::ReadAt(uint64_t offset, size_t size) {
  std::vector<uint8_t> buffer(size);
  if (size > 0) {
    ReadAt(offset, buffer.data(), size);
  }
  return buffer;
}

std::string Trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string ToLower(const std::string& text) {
  std::string result = text;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::vector<std::string> Split(const std::string& text, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream stream(text);
  while (std::getline(stream, current, delimiter)) {
    parts.push_back(current);
  }
  return parts;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ReadTextFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  Check(stream.good(), "cannot open file: " + path);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

bool LikeMatch(const char* text, size_t text_size, const std::string& pattern) {
  size_t text_index = 0;
  size_t pattern_index = 0;
  size_t star_pattern = std::string::npos;
  size_t star_text = 0;
  while (text_index < text_size) {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '_' || pattern[pattern_index] == text[text_index])) {
      ++text_index;
      ++pattern_index;
    } else if (pattern_index < pattern.size() && pattern[pattern_index] == '%') {
      star_pattern = pattern_index++;
      star_text = text_index;
    } else if (star_pattern != std::string::npos) {
      pattern_index = star_pattern + 1;
      text_index = ++star_text;
    } else {
      return false;
    }
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '%') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

}
