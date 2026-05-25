#pragma once

#include <array>
#include <cstdint>

namespace dory::race {
static constexpr uint64_t NbBucketEntries = 6;

using Value = uintptr_t;
constexpr uint64_t ValueMask = (1ULL << 48) - 1;

struct BucketEntry {
  using Checksum = uint8_t;

  BucketEntry(Checksum checksum, Value value)
      : used{true}, padding{0}, checksum{checksum}, value{value & ValueMask} {}

  uint64_t asUint64() const { return *reinterpret_cast<uint64_t const*>(this); }

  bool operator==(BucketEntry const& o) const {
    return asUint64() == o.asUint64();
  }
  Value getValue() const { return value & ValueMask; }

  bool used : 1; //flag for empty slot detection
  uint64_t padding : 7;
  Checksum checksum : 8; // 8 bit fingerprint
  Value value : 48; // 48 bit pointer to actual value
}; // 8 Bytes total for CAS ops

static_assert(sizeof(BucketEntry) == sizeof(uint64_t));
using Bucket = std::array<BucketEntry, NbBucketEntries>;  // One Bucket is an array of 6 Bucket entries (48 bytes total, fits in a cache line)
using BucketGroup = std::array<Bucket, 2>;  // BucketGroup pairs a main bucket with an overflow bucket (96 bytes total)

}  // namespace dory::race

using Key = std::string;
using KeyView = std::string_view;
using HashedKey = std::array<uint8_t, 16>;