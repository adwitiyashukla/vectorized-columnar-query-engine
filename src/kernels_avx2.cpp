#include "kernels.h"

#if VCQE_HAVE_AVX2

#include <immintrin.h>

namespace vcqe {
namespace {

inline size_t AppendMask(uint32_t mask, size_t base, uint32_t* out, size_t matched) {
  while (mask != 0) {
#if defined(_MSC_VER)
    unsigned long bit = 0;
    _BitScanForward(&bit, mask);
#else
    const unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
#endif
    out[matched++] = static_cast<uint32_t>(base + bit);
    mask &= mask - 1;
  }
  return matched;
}

inline __m256i CompareEpi32(CompareOp op, __m256i values, __m256i constant) {
  switch (op) {
  case CompareOp::kEqual:
    return _mm256_cmpeq_epi32(values, constant);
  case CompareOp::kNotEqual:
    return _mm256_xor_si256(_mm256_cmpeq_epi32(values, constant), _mm256_set1_epi32(-1));
  case CompareOp::kLess:
    return _mm256_cmpgt_epi32(constant, values);
  case CompareOp::kLessEqual:
    return _mm256_xor_si256(_mm256_cmpgt_epi32(values, constant), _mm256_set1_epi32(-1));
  case CompareOp::kGreater:
    return _mm256_cmpgt_epi32(values, constant);
  case CompareOp::kGreaterEqual:
    return _mm256_xor_si256(_mm256_cmpgt_epi32(constant, values), _mm256_set1_epi32(-1));
  }
  return _mm256_setzero_si256();
}

inline __m256i CompareEpi64(CompareOp op, __m256i values, __m256i constant) {
  switch (op) {
  case CompareOp::kEqual:
    return _mm256_cmpeq_epi64(values, constant);
  case CompareOp::kNotEqual:
    return _mm256_xor_si256(_mm256_cmpeq_epi64(values, constant), _mm256_set1_epi64x(-1));
  case CompareOp::kLess:
    return _mm256_cmpgt_epi64(constant, values);
  case CompareOp::kLessEqual:
    return _mm256_xor_si256(_mm256_cmpgt_epi64(values, constant), _mm256_set1_epi64x(-1));
  case CompareOp::kGreater:
    return _mm256_cmpgt_epi64(values, constant);
  case CompareOp::kGreaterEqual:
    return _mm256_xor_si256(_mm256_cmpgt_epi64(constant, values), _mm256_set1_epi64x(-1));
  }
  return _mm256_setzero_si256();
}

inline int DoubleCompareMode(CompareOp op) {
  switch (op) {
  case CompareOp::kEqual:
    return _CMP_EQ_OQ;
  case CompareOp::kNotEqual:
    return _CMP_NEQ_UQ;
  case CompareOp::kLess:
    return _CMP_LT_OQ;
  case CompareOp::kLessEqual:
    return _CMP_LE_OQ;
  case CompareOp::kGreater:
    return _CMP_GT_OQ;
  case CompareOp::kGreaterEqual:
    return _CMP_GE_OQ;
  }
  return _CMP_EQ_OQ;
}

template <int Mode> inline __m256d CompareDouble(__m256d values, __m256d constant) {
  return _mm256_cmp_pd(values, constant, Mode);
}

}

size_t SelectInt32Avx2(const int32_t* values, size_t count, CompareOp op, int32_t constant,
                       uint32_t* out) {
  const __m256i target = _mm256_set1_epi32(constant);
  size_t matched = 0;
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    const __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(values + i));
    const __m256i result = CompareEpi32(op, block, target);
    const uint32_t mask = static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(result)));
    if (mask != 0) {
      matched = AppendMask(mask, i, out, matched);
    }
  }
  for (; i < count; ++i) {
    if (SelectInt32Scalar(values + i, nullptr, 1, op, constant, out + matched) == 1) {
      out[matched] = static_cast<uint32_t>(i);
      ++matched;
    }
  }
  return matched;
}

size_t SelectInt64Avx2(const int64_t* values, size_t count, CompareOp op, int64_t constant,
                       uint32_t* out) {
  const __m256i target = _mm256_set1_epi64x(constant);
  size_t matched = 0;
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    const __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(values + i));
    const __m256i result = CompareEpi64(op, block, target);
    const uint32_t mask = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(result)));
    if (mask != 0) {
      matched = AppendMask(mask, i, out, matched);
    }
  }
  for (; i < count; ++i) {
    if (SelectInt64Scalar(values + i, nullptr, 1, op, constant, out + matched) == 1) {
      out[matched] = static_cast<uint32_t>(i);
      ++matched;
    }
  }
  return matched;
}

size_t SelectDoubleAvx2(const double* values, size_t count, CompareOp op, double constant,
                        uint32_t* out) {
  const __m256d target = _mm256_set1_pd(constant);
  const int mode = DoubleCompareMode(op);
  size_t matched = 0;
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    const __m256d block = _mm256_loadu_pd(values + i);
    __m256d result;
    switch (mode) {
    case _CMP_EQ_OQ:
      result = CompareDouble<_CMP_EQ_OQ>(block, target);
      break;
    case _CMP_NEQ_UQ:
      result = CompareDouble<_CMP_NEQ_UQ>(block, target);
      break;
    case _CMP_LT_OQ:
      result = CompareDouble<_CMP_LT_OQ>(block, target);
      break;
    case _CMP_LE_OQ:
      result = CompareDouble<_CMP_LE_OQ>(block, target);
      break;
    case _CMP_GT_OQ:
      result = CompareDouble<_CMP_GT_OQ>(block, target);
      break;
    default:
      result = CompareDouble<_CMP_GE_OQ>(block, target);
      break;
    }
    const uint32_t mask = static_cast<uint32_t>(_mm256_movemask_pd(result));
    if (mask != 0) {
      matched = AppendMask(mask, i, out, matched);
    }
  }
  for (; i < count; ++i) {
    if (SelectDoubleScalar(values + i, nullptr, 1, op, constant, out + matched) == 1) {
      out[matched] = static_cast<uint32_t>(i);
      ++matched;
    }
  }
  return matched;
}

int64_t SumInt64Avx2(const int64_t* values, size_t count) {
  __m256i total = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    total =
        _mm256_add_epi64(total, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(values + i)));
  }
  alignas(32) int64_t lanes[4];
  _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), total);
  int64_t result = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  for (; i < count; ++i) {
    result += values[i];
  }
  return result;
}

double SumDoubleAvx2(const double* values, size_t count) {
  __m256d total = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    total = _mm256_add_pd(total, _mm256_loadu_pd(values + i));
  }
  alignas(32) double lanes[4];
  _mm256_store_pd(lanes, total);
  double result = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  for (; i < count; ++i) {
    result += values[i];
  }
  return result;
}

}

#endif
