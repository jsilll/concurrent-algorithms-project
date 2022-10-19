#pragma once

#include <array>
#include <mutex>
#include <cmath>
#include <vector>

#include "segment.hpp"

class SegmentAllocator
{
public:
  SegmentAllocator(std::size_t size, std::size_t align)
      : align(align), shift_offset(log2(align))
  {
    all_segments = std::make_unique<SharedSegment[]>(MAX_SEGMENTS);
    available.reserve(MAX_SEGMENTS);
    for (auto i = 1; i <= MAX_SEGMENTS; ++i)
    {
      available.push_back(MAX_SEGMENTS - i);
    }
    ObjectId dummy;
    allocate(size, &dummy);
  }

  bool allocate(std::size_t size, ObjectId *addr)
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (available.empty())
    {
      return false;
    }
    auto next = available.back();
    available.pop_back();
    all_segments[next].allocate(size, align);

    *addr = ObjectId{next, 1, 0};
    return true;
  }

  inline void free(ObjectId addr)
  {
    std::unique_lock<std::mutex> lock(mutex);
    all_segments[addr.segment].deallocate();
    available.push_back(addr.segment);
  }

  inline Object &find(ObjectId addr)
  {
    const auto segm_offset = addr.offset >> shift_offset;
    return find_segment(addr)[segm_offset];
  }

  inline SharedSegment &find_segment(ObjectId addr)
  {
    return all_segments[addr.segment];
  }

  const SharedSegment &first_segment() const noexcept
  {
    return all_segments[0];
  }

  ObjectId first_addr() const noexcept { return ObjectId{0, 1, 0}; }

private:
  static constexpr std::uint8_t MAX_SEGMENTS = 255;
  
  std::size_t align, shift_offset = 0;

  std::mutex mutex;
  std::unique_ptr<SharedSegment[]> all_segments;
  std::vector<std::uint8_t> available;
};
