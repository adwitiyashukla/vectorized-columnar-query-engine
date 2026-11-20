#include <random>
#include <vector>

#include "harness.h"
#include "hash_table.h"
#include "kernels.h"
#include "util.h"
#include "vector.h"

using namespace vcqe;

namespace {

std::vector<CompareOp> AllOps() {
  return {CompareOp::kEqual,     CompareOp::kNotEqual, CompareOp::kLess,
          CompareOp::kLessEqual, CompareOp::kGreater,  CompareOp::kGreaterEqual};
}

}

TEST(SimdMatchesScalar) {
  std::mt19937 random(7);
  std::vector<int32_t> small(1000);
  std::vector<int64_t> wide(1000);
  std::vector<double> reals(1000);
  for (size_t i = 0; i < small.size(); ++i) {
    small[i] = static_cast<int32_t>(random() % 50) - 25;
    wide[i] = static_cast<int64_t>(random() % 50) - 25;
    reals[i] = static_cast<double>(random() % 50) - 25.0;
  }
  std::vector<uint32_t> fast(small.size());
  std::vector<uint32_t> slow(small.size());
  for (CompareOp op : AllOps()) {
    for (size_t count : {0u, 1u, 3u, 7u, 8u, 9u, 999u}) {
      SetSimdEnabled(true);
      const size_t fast_count = SelectInt32(small.data(), nullptr, count, op, 0, fast.data());
      SetSimdEnabled(false);
      const size_t slow_count = SelectInt32(small.data(), nullptr, count, op, 0, slow.data());
      EXPECT_EQ(fast_count, slow_count);
      EXPECT(std::equal(fast.begin(), fast.begin() + fast_count, slow.begin()));

      SetSimdEnabled(true);
      const size_t fast_wide = SelectInt64(wide.data(), nullptr, count, op, 0, fast.data());
      SetSimdEnabled(false);
      const size_t slow_wide = SelectInt64(wide.data(), nullptr, count, op, 0, slow.data());
      EXPECT_EQ(fast_wide, slow_wide);
      EXPECT(std::equal(fast.begin(), fast.begin() + fast_wide, slow.begin()));

      SetSimdEnabled(true);
      const size_t fast_real = SelectDouble(reals.data(), nullptr, count, op, 0.0, fast.data());
      SetSimdEnabled(false);
      const size_t slow_real = SelectDouble(reals.data(), nullptr, count, op, 0.0, slow.data());
      EXPECT_EQ(fast_real, slow_real);
      EXPECT(std::equal(fast.begin(), fast.begin() + fast_real, slow.begin()));
    }
  }
  SetSimdEnabled(true);
}

TEST(SelectionVectorIsHonored) {
  std::vector<int64_t> values{5, 1, 5, 9, 5};
  std::vector<uint32_t> selection{1, 2, 4};
  std::vector<uint32_t> out(5);
  const size_t matched = SelectInt64(values.data(), selection.data(), selection.size(),
                                     CompareOp::kEqual, 5, out.data());
  EXPECT_EQ(matched, 2u);
  EXPECT_EQ(out[0], 2u);
  EXPECT_EQ(out[1], 4u);
}

TEST(BitPackingRoundTrip) {
  std::mt19937_64 random(11);
  for (uint32_t width = 0; width <= 64; ++width) {
    const size_t count = 257;
    std::vector<uint64_t> values(count);
    for (size_t i = 0; i < count; ++i) {
      values[i] = width == 0 ? 0 : (random() >> (64 - width));
    }
    std::vector<uint8_t> packed;
    BitPack(values.data(), count, width, &packed);
    std::vector<uint64_t> restored(count, 0);
    BitUnpack(packed.data(), packed.size(), count, width, restored.data());
    EXPECT(values == restored);
  }
}

TEST(ChecksumDetectsChange) {
  const std::string text = "the quick brown fox";
  const uint32_t first = Crc32c(text.data(), text.size());
  std::string other = text;
  other[3] = 'X';
  EXPECT(first != Crc32c(other.data(), other.size()));
  EXPECT_EQ(first, Crc32c(text.data(), text.size()));
}

TEST(LikePatterns) {
  const std::string text = "special packages";
  EXPECT(LikeMatch(text.data(), text.size(), "%packages"));
  EXPECT(LikeMatch(text.data(), text.size(), "special%"));
  EXPECT(LikeMatch(text.data(), text.size(), "%cial pack%"));
  EXPECT(LikeMatch(text.data(), text.size(), "s%l p%s"));
  EXPECT(LikeMatch(text.data(), text.size(), "________________"));
  EXPECT(!LikeMatch(text.data(), text.size(), "_______________"));
  EXPECT(!LikeMatch(text.data(), text.size(), "%boxes"));
}

TEST(VectorValuesRoundTrip) {
  Vector vector;
  vector.Init(DataType::kDecimal, 4, 2);
  vector.SetValue(0, Value::Decimal(12345, 2));
  vector.SetValue(1, Value::Null(DataType::kDecimal));
  EXPECT_EQ(FormatValue(vector.GetValue(0)), std::string("123.45"));
  EXPECT(vector.GetValue(1).is_null);
  EXPECT(!vector.validity().all_valid());
}

TEST(HashAgreesBetweenValuesAndVectors) {
  Vector vector;
  vector.Init(DataType::kInt64, 4);
  vector.i64()[0] = 42;
  uint64_t hashes[4] = {0, 0, 0, 0};
  HashVector(vector, nullptr, 1, hashes, false);
  EXPECT_EQ(hashes[0], HashValues({Value::Int(42)}));
}

TEST(DatesRoundTrip) {
  EXPECT_EQ(FormatDate(ParseDate("1998-12-01")), std::string("1998-12-01"));
  EXPECT_EQ(FormatDate(ParseDate("1970-01-01")), std::string("1970-01-01"));
  EXPECT_EQ(ParseDate("1970-01-01"), 0);
  EXPECT_EQ(ParseDate("1992-01-02") - ParseDate("1992-01-01"), 1);
  EXPECT_THROWS(ParseDate("1992-13-01"));
  EXPECT_THROWS(ParseDate("not a date"));
}

TEST(DecimalsRoundTrip) {
  EXPECT_EQ(ParseDecimal("123.45", 2), 12345);
  EXPECT_EQ(ParseDecimal("-0.07", 2), -7);
  EXPECT_EQ(ParseDecimal("12", 2), 1200);
  EXPECT_EQ(FormatDecimal(-7, 2), std::string("-0.07"));
  EXPECT_EQ(FormatDecimal(1200, 2), std::string("12.00"));
  EXPECT_THROWS(ParseDecimal("12.3.4", 2));
}

TEST_MAIN()
