#pragma once

#include <cstdint>

#include "vector.h"

namespace vcqe {

enum class CompareOp : uint8_t {
  kEqual,
  kNotEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
};

bool HasAvx2();
bool SimdEnabled();
void SetSimdEnabled(bool enabled);

size_t SelectInt32(const int32_t* values, const uint32_t* sel, size_t count, CompareOp op,
                   int32_t constant, uint32_t* out);
size_t SelectInt64(const int64_t* values, const uint32_t* sel, size_t count, CompareOp op,
                   int64_t constant, uint32_t* out);
size_t SelectDouble(const double* values, const uint32_t* sel, size_t count, CompareOp op,
                    double constant, uint32_t* out);
size_t SelectString(const StringRef* values, const uint32_t* sel, size_t count, CompareOp op,
                    const StringRef& constant, uint32_t* out);

size_t SelectInt32Scalar(const int32_t* values, const uint32_t* sel, size_t count, CompareOp op,
                         int32_t constant, uint32_t* out);
size_t SelectInt64Scalar(const int64_t* values, const uint32_t* sel, size_t count, CompareOp op,
                         int64_t constant, uint32_t* out);
size_t SelectDoubleScalar(const double* values, const uint32_t* sel, size_t count, CompareOp op,
                          double constant, uint32_t* out);

#if VCQE_HAVE_AVX2
size_t SelectInt32Avx2(const int32_t* values, size_t count, CompareOp op, int32_t constant,
                       uint32_t* out);
size_t SelectInt64Avx2(const int64_t* values, size_t count, CompareOp op, int64_t constant,
                       uint32_t* out);
size_t SelectDoubleAvx2(const double* values, size_t count, CompareOp op, double constant,
                        uint32_t* out);
int64_t SumInt64Avx2(const int64_t* values, size_t count);
double SumDoubleAvx2(const double* values, size_t count);
#endif

int64_t SumInt64(const int64_t* values, const uint32_t* sel, size_t count);
double SumDouble(const double* values, const uint32_t* sel, size_t count);

uint64_t HashInteger(int64_t value);
uint64_t HashString(const char* data, uint32_t size);
uint64_t HashCombine(uint64_t left, uint64_t right);

void HashVector(const Vector& vector, const uint32_t* sel, size_t count, uint64_t* out,
                bool combine);

}
