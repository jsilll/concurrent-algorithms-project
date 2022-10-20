#pragma once

#include <array>
#include <cmath>
#include <mutex>
#include <vector>
#include <unordered_set>

#include "segment.hpp"

class SegmentManager
{
private:
  static constexpr std::uint8_t MAX_SEGMENTS = 255;

  std::size_t align_;
  std::size_t first_size_;
  std::size_t shift_offset_;

  std::mutex mutex_;
  std::vector<std::uint8_t> available_;
  std::unique_ptr<SharedSegment[]> all_segments_;

public:
  SegmentManager(std::size_t size, std::size_t align)
      : align_(align), first_size_(size), shift_offset_(log2(align))
  {
    all_segments_ = std::make_unique<SharedSegment[]>(MAX_SEGMENTS);
    available_.reserve(MAX_SEGMENTS);
    for (auto i = 1; i <= MAX_SEGMENTS; ++i)
    {
      available_.push_back(MAX_SEGMENTS - i);
    }
    ObjectId dummy;
    allocate(size, &dummy);
  }

public:
  static inline ObjectId start() noexcept
  {
    return ObjectId{0, 1, 0};
  }

public:
  inline SharedSegment &find_segment(const ObjectId &addr) const noexcept
  {
    return all_segments_[addr.segment];
  }

  inline const SharedSegment &first() const noexcept
  {
    return all_segments_[0];
  }

  inline std::size_t first_size() const noexcept
  {
    return first_size_;
  }

public:
  void free(ObjectId addr)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    all_segments_[addr.segment].deallocate();
    available_.push_back(addr.segment);
  }

  bool allocate(std::size_t size, ObjectId *addr)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (available_.empty())
    {
      return false;
    }
    auto next = available_.back();
    available_.pop_back();
    all_segments_[next].allocate(size, align_);

    *addr = ObjectId{next, 1, 0};
    return true;
  }

  inline Object &find(const ObjectId &addr)
  {
    return find_segment(addr)[addr.offset >> shift_offset_];
  }
};
