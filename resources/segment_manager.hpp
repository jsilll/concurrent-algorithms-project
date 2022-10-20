#pragma once

#include <array>
#include <mutex>
#include <cmath>
#include <vector>

#include "segment.hpp"

class SegmentManager
{
public:
  SegmentManager(std::size_t size, std::size_t align)
      : align_(align), shift_offset_(log2(align))
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

  inline Object &find(ObjectId addr)
  {
    const auto segm_offset = addr.offset >> shift_offset_;
    return find_segment(addr)[segm_offset];
  }

  inline SharedSegment &find_segment(ObjectId addr)
  {
    return all_segments_[addr.segment];
  }

  inline const SharedSegment &first() const noexcept
  {
    return all_segments_[0];
  }

  ObjectId start() const noexcept { return ObjectId{0, 1, 0}; }

private:
  static constexpr std::uint8_t MAX_SEGMENTS = 255;

  std::size_t align_, shift_offset_ = 0;

  std::mutex mutex_;
  std::unique_ptr<SharedSegment[]> all_segments_;
  std::vector<std::uint8_t> available_;
};
