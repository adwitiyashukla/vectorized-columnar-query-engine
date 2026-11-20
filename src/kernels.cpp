#include "kernels.h"

#include <atomic>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace vcqe {
namespace {

std::atomic<bool> g_simd_enabled{true};

template <typename T> bool ApplyOp(CompareOp op, const T& left, const T& right) {
  switch (op) {
  case CompareOp::kEqual:
    return left == right;
  case CompareOp::kNotEqual:
    return left != right;
  case CompareOp::kLess:
    return left < right;
  case CompareOp::kLessEqual:
    return left <= right;
  case CompareOp::kGreater:
    return left > right;
  case CompareOp::kGreaterEqual:
    return left >= right;
  }
  return false;
}

template <typename T>
size_t SelectTyped(const T* values, const uint32_t* sel, size_t count, CompareOp op, T constant,
                   uint32_t* out) {
  size_t matched = 0;
  if (sel == nullptr) {
    for (size_t i = 0; i < count; ++i) {
      if (ApplyOp(op, values[i], constant)) {
        out[matched++] = static_cast<uint32_t>(i);
      }
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      const uint32_t row = sel[i];
      if (ApplyOp(op, values[row], constant)) {
        out[matched++] = row;
      }
    }
  }
  return matched;
}

bool DetectAvx2() {
#if defined(_MSC_VER)
  int registers[4] = {0, 0, 0, 0};
  __cpuid(registers, 0);
  if (registers[0] < 7) {
    return false;
  }
  __cpuidex(registers, 7, 0);
  return (registers[1] & (1 << 5)) != 0;
#elif defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}

}

bool HasAvx2() {
  static const bool detected = DetectAvx2();
  return detected;
}

bool SimdEnabled() {
  return g_simd_enabled.load(std::memory_order_relaxed) && HasAvx2();
}

void SetSimdEnabled(bool enabled) {
  g_simd_enabled.store(enabled, std::memory_order_relaxed);
}

size_t SelectInt32Scalar(const int32_t* values, const uint32_t* sel, size_t count, CompareOp op,
                         int32_t constant, uint32_t* out) {
  return SelectTyped(values, sel, count, op, constant, out);
}

size_t SelectInt64Scalar(const int64_t* values, const uint32_t* sel, size_t count, CompareOp op,
                         int64_t constant, uint32_t* out) {
  return SelectTyped(values, sel, count, op, constant, out);
}

size_t SelectDoubleScalar(const double* values, const uint32_t* sel, size_t count, CompareOp op,
                          double constant, uint32_t* out) {
  return SelectTyped(values, sel, count, op, constant, out);
}

size_t SelectInt32(const int32_t* values, const uint32_t* sel, size_t count, CompareOp op,
                   int32_t constant, uint32_t* out) {
#if VCQE_HAVE_AVX2
  if (sel == nullptr && SimdEnabled()) {
    return SelectInt32Avx2(values, count, op, constant, out);
  }
#endif
  return SelectInt32Scalar(values, sel, count, op, constant, out);
}

size_t SelectInt64(const int64_t* values, const uint32_t* sel, size_t count, CompareOp op,
                   int64_t constant, uint32_t* out) {
#if VCQE_HAVE_AVX2
  if (sel == nullptr && SimdEnabled()) {
    return SelectInt64Avx2(values, count, op, constant, out);
  }
#endif
  return SelectInt64Scalar(values, sel, count, op, constant, out);
}

size_t SelectDouble(const double* values, const uint32_t* sel, size_t count, CompareOp op,
                    double constant, uint32_t* out) {
#if VCQE_HAVE_AVX2
  if (sel == nullptr && SimdEnabled()) {
    return SelectDoubleAvx2(values, count, op, constant, out);
  }
#endif
  return SelectDoubleScalar(values, sel, count, op, constant, out);
}

size_t SelectString(const StringRef* values, const uint32_t* sel, size_t count, CompareOp op,
                    const StringRef& constant, uint32_t* out) {
  size_t matched = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t row = sel == nullptr ? static_cast<uint32_t>(i) : sel[i];
    bool keep = false;
    if (op == CompareOp::kEqual) {
      keep = values[row].Equals(constant);
    } else if (op == CompareOp::kNotEqual) {
      keep = !values[row].Equals(constant);
    } else {
      keep = ApplyOp(op, values[row].Compare(constant), 0);
    }
    if (keep) {
      out[matched++] = row;
    }
  }
  return matched;
}

int64_t SumInt64(const int64_t* values, const uint32_t* sel, size_t count) {
#if VCQE_HAVE_AVX2
  if (sel == nullptr && SimdEnabled()) {
    return SumInt64Avx2(values, count);
  }
#endif
  int64_t total = 0;
  if (sel == nullptr) {
    for (size_t i = 0; i < count; ++i) {
      total += values[i];
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      total += values[sel[i]];
    }
  }
  return total;
}

double SumDouble(const double* values, const uint32_t* sel, size_t count) {
#if VCQE_HAVE_AVX2
  if (sel == nullptr && SimdEnabled()) {
    return SumDoubleAvx2(values, count);
  }
#endif
  double total = 0.0;
  if (sel == nullptr) {
    for (size_t i = 0; i < count; ++i) {
      total += values[i];
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      total += values[sel[i]];
    }
  }
  return total;
}

uint64_t HashInteger(int64_t value) {
  uint64_t x = static_cast<uint64_t>(value);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

uint64_t HashString(const char* data, uint32_t size) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < size; ++i) {
    hash ^= static_cast<uint8_t>(data[i]);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

uint64_t HashCombine(uint64_t left, uint64_t right) {
  return left ^ (right + 0x9e3779b97f4a7c15ull + (left << 6) + (left >> 2));
}

void HashVector(const Vector& vector, const uint32_t* sel, size_t count, uint64_t* out,
                bool combine) {
  const DataType type = vector.type();
  for (size_t i = 0; i < count; ++i) {
    const uint32_t row = sel == nullptr ? static_cast<uint32_t>(i) : sel[i];
    uint64_t hash = 0;
    if (!vector.validity().Get(row)) {
      hash = 0x9ae16a3b2f90404full;
    } else if (type == DataType::kString) {
      const StringRef& text = vector.strings()[row];
      hash = HashString(text.data, text.size);
    } else if (type == DataType::kDouble) {
      double value = vector.f64()[row];
      int64_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      hash = HashInteger(bits);
    } else {
      hash = HashInteger(vector.GetInteger(row));
    }
    out[i] = combine ? HashCombine(out[i], hash) : hash;
  }
}

}
