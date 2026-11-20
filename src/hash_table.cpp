#include "hash_table.h"

#include <cstring>
#include <limits>
#include <type_traits>

#include "error.h"
#include "kernels.h"

namespace vcqe {
namespace {

constexpr size_t kNullMaskOffset = 16;

size_t RoundUpPowerOfTwo(size_t value) {
  size_t result = 16;
  while (result < value) {
    result <<= 1;
  }
  return result;
}

}

uint64_t HashValues(const std::vector<Value>& values) {
  uint64_t hash = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    const Value& value = values[i];
    uint64_t current = 0;
    if (value.is_null) {
      current = 0x9ae16a3b2f90404full;
    } else if (value.type == DataType::kString) {
      current = HashString(value.text.data(), static_cast<uint32_t>(value.text.size()));
    } else if (value.type == DataType::kDouble) {
      int64_t bits = 0;
      std::memcpy(&bits, &value.real, sizeof(bits));
      current = HashInteger(bits);
    } else {
      current = HashInteger(value.integer);
    }
    hash = i == 0 ? current : HashCombine(hash, current);
  }
  return hash;
}

void RowStore::Init(std::vector<DataType> key_types, size_t state_bytes, size_t initial_buckets,
                    double load_factor) {
  key_types_ = std::move(key_types);
  state_offset_ = kHeaderBytes + key_types_.size() * 8;
  row_width_ = state_offset_ + state_bytes;
  load_factor_ = load_factor;
  row_count_ = 0;
  capacity_ = 0;
  blocks_.clear();
  arena_.clear();
  const size_t buckets = RoundUpPowerOfTwo(initial_buckets);
  buckets_.assign(buckets, 0);
  bucket_mask_ = buckets - 1;
}

uint64_t RowStore::RowHash(const uint8_t* row) {
  uint64_t hash = 0;
  std::memcpy(&hash, row, sizeof(hash));
  return hash;
}

uint32_t RowStore::RowNext(const uint8_t* row) {
  uint32_t next = 0;
  std::memcpy(&next, row + 8, sizeof(next));
  return next;
}

StringRef RowStore::SlotText(const uint8_t* slot) const {
  uint32_t offset = 0;
  uint32_t size = 0;
  std::memcpy(&offset, slot, sizeof(offset));
  std::memcpy(&size, slot + 4, sizeof(size));
  return StringRef{arena_.data() + offset, size};
}

void RowStore::StoreSlot(uint8_t* slot, uint8_t* null_mask, size_t index, DataType type,
                         const Vector& source, size_t source_row) {
  if (!source.validity().Get(source_row)) {
    uint64_t mask = 0;
    std::memcpy(&mask, null_mask, sizeof(mask));
    mask |= 1ull << index;
    std::memcpy(null_mask, &mask, sizeof(mask));
    std::memset(slot, 0, 8);
    return;
  }
  if (type == DataType::kString) {
    const StringRef& text = source.strings()[source_row];
    const uint32_t offset = static_cast<uint32_t>(arena_.size());
    arena_.insert(arena_.end(), text.data, text.data + text.size);
    std::memcpy(slot, &offset, sizeof(offset));
    std::memcpy(slot + 4, &text.size, sizeof(text.size));
    return;
  }
  if (type == DataType::kDouble) {
    const double value = source.f64()[source_row];
    std::memcpy(slot, &value, sizeof(value));
    return;
  }
  const int64_t value = source.GetInteger(source_row);
  std::memcpy(slot, &value, sizeof(value));
}

void RowStore::StoreSlotValue(uint8_t* slot, uint8_t* null_mask, size_t index, DataType type,
                              const Value& value) {
  if (value.is_null) {
    uint64_t mask = 0;
    std::memcpy(&mask, null_mask, sizeof(mask));
    mask |= 1ull << index;
    std::memcpy(null_mask, &mask, sizeof(mask));
    std::memset(slot, 0, 8);
    return;
  }
  if (type == DataType::kString) {
    const uint32_t offset = static_cast<uint32_t>(arena_.size());
    const uint32_t size = static_cast<uint32_t>(value.text.size());
    arena_.insert(arena_.end(), value.text.begin(), value.text.end());
    std::memcpy(slot, &offset, sizeof(offset));
    std::memcpy(slot + 4, &size, sizeof(size));
    return;
  }
  if (type == DataType::kDouble) {
    std::memcpy(slot, &value.real, sizeof(value.real));
    return;
  }
  std::memcpy(slot, &value.integer, sizeof(value.integer));
}

Value RowStore::LoadSlot(const uint8_t* slot, const uint8_t* null_mask, size_t index, DataType type,
                         uint8_t scale) const {
  uint64_t mask = 0;
  std::memcpy(&mask, null_mask, sizeof(mask));
  if ((mask >> index) & 1ull) {
    return Value::Null(type);
  }
  switch (type) {
  case DataType::kString: {
    const StringRef text = SlotText(slot);
    return Value::String(text.ToString());
  }
  case DataType::kDouble: {
    double value = 0.0;
    std::memcpy(&value, slot, sizeof(value));
    return Value::Real(value);
  }
  case DataType::kDecimal: {
    int64_t value = 0;
    std::memcpy(&value, slot, sizeof(value));
    return Value::Decimal(value, scale);
  }
  case DataType::kDate: {
    int64_t value = 0;
    std::memcpy(&value, slot, sizeof(value));
    return Value::Date(static_cast<int32_t>(value));
  }
  default: {
    int64_t value = 0;
    std::memcpy(&value, slot, sizeof(value));
    return Value::Int(value, type);
  }
  }
}

bool RowStore::KeysEqual(const uint8_t* row, const std::vector<const Vector*>& keys,
                         size_t source_row) {
  uint64_t mask = 0;
  std::memcpy(&mask, row + kNullMaskOffset, sizeof(mask));
  for (size_t i = 0; i < keys.size(); ++i) {
    const Vector& source = *keys[i];
    const bool source_null = !source.validity().Get(source_row);
    const bool stored_null = ((mask >> i) & 1ull) != 0;
    if (source_null || stored_null) {
      if (source_null != stored_null) {
        return false;
      }
      continue;
    }
    const uint8_t* slot = KeySlot(row, i);
    if (key_types_[i] == DataType::kString) {
      const StringRef stored = SlotText(slot);
      if (!stored.Equals(source.strings()[source_row])) {
        return false;
      }
    } else if (key_types_[i] == DataType::kDouble) {
      double stored = 0.0;
      std::memcpy(&stored, slot, sizeof(stored));
      if (stored != source.f64()[source_row]) {
        return false;
      }
    } else {
      int64_t stored = 0;
      std::memcpy(&stored, slot, sizeof(stored));
      if (stored != source.GetInteger(source_row)) {
        return false;
      }
    }
  }
  return true;
}

bool RowStore::KeysEqualValues(const uint8_t* row, const std::vector<Value>& keys) {
  uint64_t mask = 0;
  std::memcpy(&mask, row + kNullMaskOffset, sizeof(mask));
  for (size_t i = 0; i < keys.size(); ++i) {
    const bool stored_null = ((mask >> i) & 1ull) != 0;
    if (keys[i].is_null || stored_null) {
      if (keys[i].is_null != stored_null) {
        return false;
      }
      continue;
    }
    const uint8_t* slot = KeySlot(row, i);
    if (key_types_[i] == DataType::kString) {
      const StringRef stored = SlotText(slot);
      if (stored.size != keys[i].text.size() ||
          std::memcmp(stored.data, keys[i].text.data(), stored.size) != 0) {
        return false;
      }
    } else if (key_types_[i] == DataType::kDouble) {
      double stored = 0.0;
      std::memcpy(&stored, slot, sizeof(stored));
      if (stored != keys[i].real) {
        return false;
      }
    } else {
      int64_t stored = 0;
      std::memcpy(&stored, slot, sizeof(stored));
      if (stored != keys[i].integer) {
        return false;
      }
    }
  }
  return true;
}

void RowStore::Grow() {
  const size_t buckets = (buckets_.size() == 0 ? 16 : buckets_.size()) * 2;
  buckets_.assign(buckets, 0);
  bucket_mask_ = buckets - 1;
  for (size_t i = 0; i < row_count_; ++i) {
    uint8_t* row = RowAt(i);
    const uint64_t hash = RowHash(row);
    const size_t bucket = BucketOf(hash);
    const uint32_t head = buckets_[bucket];
    std::memcpy(row + 8, &head, sizeof(head));
    buckets_[bucket] = static_cast<uint32_t>(i + 1);
  }
}

size_t RowStore::Append(uint64_t hash) {
  if (static_cast<double>(row_count_ + 1) > static_cast<double>(buckets_.size()) * load_factor_) {
    Grow();
  }
  if (row_count_ == capacity_) {
    blocks_.emplace_back(kBlockRows * row_width_, 0);
    capacity_ += kBlockRows;
  }
  const size_t index = row_count_++;
  uint8_t* row = RowAt(index);
  std::memset(row, 0, row_width_);
  std::memcpy(row, &hash, sizeof(hash));
  const size_t bucket = BucketOf(hash);
  const uint32_t head = buckets_[bucket];
  std::memcpy(row + 8, &head, sizeof(head));
  buckets_[bucket] = static_cast<uint32_t>(index + 1);
  return index;
}

void JoinHashTable::Init(std::vector<DataType> key_types, std::vector<ColumnSchema> payload,
                         size_t initial_buckets, double load_factor) {
  payload_ = std::move(payload);
  store_.Init(std::move(key_types), 8 + payload_.size() * 8, initial_buckets, load_factor);
}

void JoinHashTable::Insert(const std::vector<const Vector*>& keys,
                           const std::vector<const Vector*>& payload, const uint32_t* sel,
                           size_t count, uint64_t* hashes) {
  for (size_t i = 0; i < keys.size(); ++i) {
    HashVector(*keys[i], sel, count, hashes, i > 0);
  }
  for (size_t i = 0; i < count; ++i) {
    const size_t source_row = sel == nullptr ? i : sel[i];
    const size_t index = store_.Append(hashes[i]);
    uint8_t* row = store_.RowAt(index);
    uint8_t* key_mask = row + kNullMaskOffset;
    for (size_t k = 0; k < keys.size(); ++k) {
      store_.StoreSlot(store_.KeySlot(row, k), key_mask, k, store_.key_types()[k], *keys[k],
                       source_row);
    }
    uint8_t* state = store_.StateBytes(row);
    for (size_t p = 0; p < payload.size(); ++p) {
      store_.StoreSlot(state + 8 + p * 8, state, p, payload_[p].type, *payload[p], source_row);
    }
  }
}

void JoinHashTable::Gather(const uint8_t* row, size_t payload_index, Vector& target,
                           size_t target_row) const {
  const uint8_t* state = store_.StateBytes(row);
  const uint8_t* slot = state + 8 + payload_index * 8;
  uint64_t mask = 0;
  std::memcpy(&mask, state, sizeof(mask));
  if ((mask >> payload_index) & 1ull) {
    target.validity().SetNull(target_row);
    return;
  }
  const DataType type = payload_[payload_index].type;
  if (type == DataType::kString) {
    const StringRef text = store_.SlotText(slot);
    target.AppendString(target_row, text.data, text.size);
    return;
  }
  if (type == DataType::kDouble) {
    double value = 0.0;
    std::memcpy(&value, slot, sizeof(value));
    target.f64()[target_row] = value;
    return;
  }
  int64_t value = 0;
  std::memcpy(&value, slot, sizeof(value));
  if (TypeWidth(type) == 4) {
    target.i32()[target_row] = static_cast<int32_t>(value);
  } else {
    target.i64()[target_row] = value;
  }
}

size_t JoinHashTable::Probe(const std::vector<const Vector*>& keys, const uint32_t* sel,
                            size_t count, const uint64_t* hashes, ProbeState* state,
                            uint32_t* probe_rows, const uint8_t** build_rows, size_t limit) {
  size_t produced = 0;
  while (state->position < count && produced < limit) {
    const size_t i = state->position;
    const size_t source_row = sel == nullptr ? i : sel[i];
    if (!state->walking) {
      state->chain = store_.Head(store_.BucketOf(hashes[i]));
      state->walking = true;
    }
    uint32_t candidate = state->chain;
    while (candidate != 0) {
      const uint8_t* row = store_.RowAt(candidate - 1);
      const uint32_t next = RowStore::RowNext(row);
      if (RowStore::RowHash(row) == hashes[i] && store_.KeysEqual(row, keys, source_row)) {
        probe_rows[produced] = static_cast<uint32_t>(source_row);
        build_rows[produced] = row;
        ++produced;
        if (produced == limit) {
          state->chain = next;
          return produced;
        }
      }
      candidate = next;
    }
    state->chain = 0;
    state->walking = false;
    ++state->position;
  }
  return produced;
}

void AggregateTable::Init(std::vector<DataType> key_types, std::vector<AggregateSpec> aggregates,
                          size_t initial_buckets, double load_factor) {
  aggregates_ = std::move(aggregates);
  store_.Init(std::move(key_types), sizeof(State) * aggregates_.size(), initial_buckets,
              load_factor);
  state_offset_ = store_.state_offset();
}

void AggregateTable::UpdateState(State* state, size_t aggregate, const Value& value) {
  const AggregateSpec& spec = aggregates_[aggregate];
  switch (spec.kind) {
  case AggregateKind::kCountStar:
    ++state->count;
    return;
  case AggregateKind::kCount:
    if (!value.is_null) {
      ++state->count;
    }
    return;
  case AggregateKind::kSum:
    if (!value.is_null) {
      if (spec.input_type == DataType::kDouble) {
        state->real += value.real;
      } else {
        state->integer += value.integer;
      }
      ++state->count;
    }
    return;
  case AggregateKind::kAvg:
    if (!value.is_null) {
      if (spec.input_type == DataType::kDouble) {
        state->real += value.real;
      } else if (spec.input_type == DataType::kDecimal) {
        state->real += static_cast<double>(value.integer);
      } else {
        state->real += static_cast<double>(value.integer);
      }
      ++state->count;
    }
    return;
  case AggregateKind::kMin:
  case AggregateKind::kMax: {
    if (value.is_null) {
      return;
    }
    const bool first = state->count == 0;
    if (spec.input_type == DataType::kDouble) {
      if (first || (spec.kind == AggregateKind::kMin ? value.real < state->real
                                                     : value.real > state->real)) {
        state->real = value.real;
      }
    } else {
      if (first || (spec.kind == AggregateKind::kMin ? value.integer < state->integer
                                                     : value.integer > state->integer)) {
        state->integer = value.integer;
      }
    }
    ++state->count;
    return;
  }
  }
}

uint8_t* AggregateTable::FindOrCreateValues(const std::vector<Value>& keys, uint64_t hash) {
  uint32_t candidate = store_.Head(store_.BucketOf(hash));
  while (candidate != 0) {
    uint8_t* row = store_.RowAt(candidate - 1);
    if (RowStore::RowHash(row) == hash && store_.KeysEqualValues(row, keys)) {
      return row;
    }
    candidate = RowStore::RowNext(row);
  }
  const size_t index = store_.Append(hash);
  uint8_t* row = store_.RowAt(index);
  uint8_t* mask = row + kNullMaskOffset;
  for (size_t i = 0; i < keys.size(); ++i) {
    store_.StoreSlotValue(store_.KeySlot(row, i), mask, i, store_.key_types()[i], keys[i]);
  }
  return row;
}

template <typename T>
void AggregateTable::ApplyTyped(AggregateKind kind, const T* values, const Vector& source,
                                const uint32_t* sel, size_t count,
                                const std::vector<uint8_t*>& pointers, size_t index) {
  const bool all_valid = source.validity().all_valid();
  const size_t offset = index * sizeof(State);
  for (size_t i = 0; i < count; ++i) {
    const size_t row = sel == nullptr ? i : sel[i];
    if (!all_valid && !source.validity().Get(row)) {
      continue;
    }
    State* state = reinterpret_cast<State*>(pointers[i] + state_offset_ + offset);
    const T value = values[row];
    switch (kind) {
    case AggregateKind::kCount:
      ++state->count;
      break;
    case AggregateKind::kSum:
      if constexpr (std::is_floating_point<T>::value) {
        state->real += value;
      } else {
        state->integer += static_cast<int64_t>(value);
      }
      ++state->count;
      break;
    case AggregateKind::kAvg:
      state->real += static_cast<double>(value);
      ++state->count;
      break;
    case AggregateKind::kMin:
      if constexpr (std::is_floating_point<T>::value) {
        if (state->count == 0 || value < state->real) {
          state->real = value;
        }
      } else {
        if (state->count == 0 || static_cast<int64_t>(value) < state->integer) {
          state->integer = static_cast<int64_t>(value);
        }
      }
      ++state->count;
      break;
    case AggregateKind::kMax:
      if constexpr (std::is_floating_point<T>::value) {
        if (state->count == 0 || value > state->real) {
          state->real = value;
        }
      } else {
        if (state->count == 0 || static_cast<int64_t>(value) > state->integer) {
          state->integer = static_cast<int64_t>(value);
        }
      }
      ++state->count;
      break;
    case AggregateKind::kCountStar:
      ++state->count;
      break;
    }
  }
}

void AggregateTable::AddBatch(const std::vector<const Vector*>& keys,
                              const std::vector<const Vector*>& inputs, const uint32_t* sel,
                              size_t count, uint64_t* hashes) {
  if (count == 0) {
    return;
  }
  if (keys.empty()) {
    for (size_t i = 0; i < count; ++i) {
      hashes[i] = 0;
    }
  } else {
    for (size_t i = 0; i < keys.size(); ++i) {
      HashVector(*keys[i], sel, count, hashes, i > 0);
    }
  }
  pointers_.resize(count);
  for (size_t i = 0; i < count; ++i) {
    const size_t source_row = sel == nullptr ? i : sel[i];
    const uint64_t hash = hashes[i];
    uint32_t candidate = store_.Head(store_.BucketOf(hash));
    uint8_t* found = nullptr;
    while (candidate != 0) {
      uint8_t* row = store_.RowAt(candidate - 1);
      if (RowStore::RowHash(row) == hash && store_.KeysEqual(row, keys, source_row)) {
        found = row;
        break;
      }
      candidate = RowStore::RowNext(row);
    }
    if (found == nullptr) {
      const size_t index = store_.Append(hash);
      found = store_.RowAt(index);
      uint8_t* mask = found + kNullMaskOffset;
      for (size_t k = 0; k < keys.size(); ++k) {
        store_.StoreSlot(store_.KeySlot(found, k), mask, k, store_.key_types()[k], *keys[k],
                         source_row);
      }
    }
    pointers_[i] = found;
  }
  for (size_t a = 0; a < aggregates_.size(); ++a) {
    const AggregateSpec& spec = aggregates_[a];
    const Vector* input = inputs[a];
    if (spec.kind == AggregateKind::kCountStar || input == nullptr) {
      for (size_t i = 0; i < count; ++i) {
        ++(reinterpret_cast<State*>(store_.StateBytes(pointers_[i])) + a)->count;
      }
      continue;
    }
    switch (input->type()) {
    case DataType::kInt32:
    case DataType::kDate:
      ApplyTyped<int32_t>(spec.kind, input->i32(), *input, sel, count, pointers_, a);
      break;
    case DataType::kInt64:
    case DataType::kDecimal:
      ApplyTyped<int64_t>(spec.kind, input->i64(), *input, sel, count, pointers_, a);
      break;
    case DataType::kDouble:
      ApplyTyped<double>(spec.kind, input->f64(), *input, sel, count, pointers_, a);
      break;
    case DataType::kString:
      for (size_t i = 0; i < count; ++i) {
        const size_t source_row = sel == nullptr ? i : sel[i];
        State* state = reinterpret_cast<State*>(store_.StateBytes(pointers_[i])) + a;
        UpdateState(state, a, input->GetValue(source_row));
      }
      break;
    }
  }
}

void AggregateTable::AddRow(const std::vector<Value>& keys, const std::vector<Value>& inputs) {
  uint8_t* row = FindOrCreateValues(keys, HashValues(keys));
  State* states = reinterpret_cast<State*>(store_.StateBytes(row));
  for (size_t a = 0; a < aggregates_.size(); ++a) {
    UpdateState(states + a, a, inputs[a]);
  }
}

void AggregateTable::MergeState(State* target, const State& source, size_t aggregate) {
  const AggregateSpec& spec = aggregates_[aggregate];
  switch (spec.kind) {
  case AggregateKind::kCountStar:
  case AggregateKind::kCount:
    target->count += source.count;
    return;
  case AggregateKind::kSum:
  case AggregateKind::kAvg:
    target->integer += source.integer;
    target->real += source.real;
    target->count += source.count;
    return;
  case AggregateKind::kMin:
  case AggregateKind::kMax: {
    if (source.count == 0) {
      return;
    }
    if (target->count == 0) {
      target->integer = source.integer;
      target->real = source.real;
    } else if (spec.input_type == DataType::kDouble) {
      if (spec.kind == AggregateKind::kMin ? source.real < target->real
                                           : source.real > target->real) {
        target->real = source.real;
      }
    } else {
      if (spec.kind == AggregateKind::kMin ? source.integer < target->integer
                                           : source.integer > target->integer) {
        target->integer = source.integer;
      }
    }
    target->count += source.count;
    return;
  }
  }
}

void AggregateTable::MergeFrom(const AggregateTable& other) {
  const size_t keys = store_.key_types().size();
  for (size_t group = 0; group < other.group_count(); ++group) {
    std::vector<Value> key_values;
    key_values.reserve(keys);
    for (size_t k = 0; k < keys; ++k) {
      key_values.push_back(other.GroupKey(group, k));
    }
    uint8_t* row = FindOrCreateValues(key_values, HashValues(key_values));
    State* target = reinterpret_cast<State*>(store_.StateBytes(row));
    const State* source =
        reinterpret_cast<const State*>(other.store_.StateBytes(other.store_.RowAt(group)));
    for (size_t a = 0; a < aggregates_.size(); ++a) {
      MergeState(target + a, source[a], a);
    }
  }
}

Value AggregateTable::GroupKey(size_t group, size_t key) const {
  const uint8_t* row = store_.RowAt(group);
  return store_.LoadSlot(store_.KeySlot(row, key), row + kNullMaskOffset, key,
                         store_.key_types()[key], 0);
}

Value AggregateTable::Result(size_t group, size_t aggregate) const {
  const uint8_t* row = store_.RowAt(group);
  const State* states = reinterpret_cast<const State*>(store_.StateBytes(row));
  const State& state = states[aggregate];
  const AggregateSpec& spec = aggregates_[aggregate];
  switch (spec.kind) {
  case AggregateKind::kCountStar:
  case AggregateKind::kCount:
    return Value::Int(state.count, DataType::kInt64);
  case AggregateKind::kSum:
    if (state.count == 0) {
      return Value::Null(spec.output_type);
    }
    if (spec.input_type == DataType::kDouble) {
      return Value::Real(state.real);
    }
    if (spec.input_type == DataType::kDecimal) {
      return Value::Decimal(state.integer, spec.output_scale);
    }
    return Value::Int(state.integer, DataType::kInt64);
  case AggregateKind::kAvg: {
    if (state.count == 0) {
      return Value::Null(DataType::kDouble);
    }
    double total = state.real;
    if (spec.input_type == DataType::kDecimal) {
      double factor = 1.0;
      for (uint8_t i = 0; i < spec.input_scale; ++i) {
        factor *= 10.0;
      }
      total /= factor;
    }
    return Value::Real(total / static_cast<double>(state.count));
  }
  case AggregateKind::kMin:
  case AggregateKind::kMax: {
    if (state.count == 0) {
      return Value::Null(spec.output_type);
    }
    if (spec.input_type == DataType::kDouble) {
      return Value::Real(state.real);
    }
    if (spec.input_type == DataType::kDecimal) {
      return Value::Decimal(state.integer, spec.output_scale);
    }
    if (spec.input_type == DataType::kDate) {
      return Value::Date(static_cast<int32_t>(state.integer));
    }
    return Value::Int(state.integer, spec.output_type);
  }
  }
  return Value::Null(spec.output_type);
}

}
