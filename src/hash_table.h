#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "expr.h"
#include "types.h"
#include "vector.h"

namespace vcqe {

enum class AggregateKind : uint8_t { kCountStar, kCount, kSum, kMin, kMax, kAvg };

struct AggregateSpec {
  AggregateKind kind = AggregateKind::kCountStar;
  ExprPtr input;
  DataType input_type = DataType::kInt64;
  uint8_t input_scale = 0;
  DataType output_type = DataType::kInt64;
  uint8_t output_scale = 0;
  std::string name;
};

uint64_t HashValues(const std::vector<Value>& values);

class RowStore {
public:
  void Init(std::vector<DataType> key_types, size_t state_bytes, size_t initial_buckets,
            double load_factor);

  size_t row_count() const {
    return row_count_;
  }
  size_t state_offset() const {
    return state_offset_;
  }
  const std::vector<DataType>& key_types() const {
    return key_types_;
  }
  std::vector<char>& arena() {
    return arena_;
  }
  const std::vector<char>& arena() const {
    return arena_;
  }

  uint8_t* RowAt(size_t index) {
    return blocks_[index / kBlockRows].data() + (index % kBlockRows) * row_width_;
  }
  const uint8_t* RowAt(size_t index) const {
    return blocks_[index / kBlockRows].data() + (index % kBlockRows) * row_width_;
  }
  static uint64_t RowHash(const uint8_t* row);
  static uint32_t RowNext(const uint8_t* row);
  uint8_t* KeySlot(uint8_t* row, size_t key) const {
    return row + kHeaderBytes + key * 8;
  }
  const uint8_t* KeySlot(const uint8_t* row, size_t key) const {
    return row + kHeaderBytes + key * 8;
  }
  uint8_t* StateBytes(uint8_t* row) const {
    return row + state_offset_;
  }
  const uint8_t* StateBytes(const uint8_t* row) const {
    return row + state_offset_;
  }

  void StoreSlot(uint8_t* slot, uint8_t* null_mask, size_t index, DataType type,
                 const Vector& source, size_t source_row);
  void StoreSlotValue(uint8_t* slot, uint8_t* null_mask, size_t index, DataType type,
                      const Value& value);
  Value LoadSlot(const uint8_t* slot, const uint8_t* null_mask, size_t index, DataType type,
                 uint8_t scale) const;
  StringRef SlotText(const uint8_t* slot) const;

  bool KeysEqual(const uint8_t* row, const std::vector<const Vector*>& keys, size_t source_row);
  bool KeysEqualValues(const uint8_t* row, const std::vector<Value>& keys);
  size_t Append(uint64_t hash);
  uint32_t Head(size_t bucket) const {
    return buckets_[bucket];
  }
  size_t BucketOf(uint64_t hash) const {
    return static_cast<size_t>(hash >> 16) & bucket_mask_;
  }

  static constexpr size_t kHeaderBytes = 24;
  static constexpr size_t kBlockRows = 4096;

private:
  void Grow();

  std::vector<std::vector<uint8_t>> blocks_;
  std::vector<uint32_t> buckets_;
  std::vector<char> arena_;
  std::vector<DataType> key_types_;
  size_t row_width_ = 0;
  size_t row_count_ = 0;
  size_t state_offset_ = 0;
  size_t bucket_mask_ = 0;
  size_t capacity_ = 0;
  double load_factor_ = 0.6;
};

struct ProbeState {
  size_t position = 0;
  uint32_t chain = 0;
  bool walking = false;

  void Reset() {
    position = 0;
    chain = 0;
    walking = false;
  }
};

class JoinHashTable {
public:
  void Init(std::vector<DataType> key_types, std::vector<ColumnSchema> payload,
            size_t initial_buckets, double load_factor);
  void Insert(const std::vector<const Vector*>& keys, const std::vector<const Vector*>& payload,
              const uint32_t* sel, size_t count, uint64_t* hashes);
  size_t row_count() const {
    return store_.row_count();
  }
  const std::vector<ColumnSchema>& payload_schema() const {
    return payload_;
  }
  void Gather(const uint8_t* row, size_t payload_index, Vector& target, size_t target_row) const;
  size_t Probe(const std::vector<const Vector*>& keys, const uint32_t* sel, size_t count,
               const uint64_t* hashes, ProbeState* state, uint32_t* probe_rows,
               const uint8_t** build_rows, size_t limit);
  bool empty() const {
    return store_.row_count() == 0;
  }

private:
  RowStore store_;
  std::vector<ColumnSchema> payload_;
};

class AggregateTable {
public:
  void Init(std::vector<DataType> key_types, std::vector<AggregateSpec> aggregates,
            size_t initial_buckets, double load_factor);
  void AddBatch(const std::vector<const Vector*>& keys, const std::vector<const Vector*>& inputs,
                const uint32_t* sel, size_t count, uint64_t* hashes);
  void AddRow(const std::vector<Value>& keys, const std::vector<Value>& inputs);
  void MergeFrom(const AggregateTable& other);
  size_t group_count() const {
    return store_.row_count();
  }
  Value GroupKey(size_t group, size_t key) const;
  Value Result(size_t group, size_t aggregate) const;
  const std::vector<AggregateSpec>& aggregates() const {
    return aggregates_;
  }

private:
  struct State {
    int64_t integer;
    double real;
    int64_t count;
  };

  uint8_t* FindOrCreateValues(const std::vector<Value>& keys, uint64_t hash);
  template <typename T>
  void ApplyTyped(AggregateKind kind, const T* values, const Vector& source, const uint32_t* sel,
                  size_t count, const std::vector<uint8_t*>& pointers, size_t index);
  void UpdateState(State* state, size_t aggregate, const Value& value);
  void MergeState(State* target, const State& source, size_t aggregate);

  RowStore store_;
  std::vector<AggregateSpec> aggregates_;
  std::vector<uint8_t*> pointers_;
  size_t state_offset_ = 0;
};

}
