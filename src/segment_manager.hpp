#pragma once

#include <array>
#include <iostream>
#include <mutex>
#include <vector>

#include "segment.hpp"

class SegmentAllocator
{
public:
  SegmentAllocator(std::size_t size, std::size_t align);

  bool allocate(std::size_t size, ObjectId *addr);
  void free(ObjectId addr);

  Object &find(ObjectId addr);
  SharedSegment &find_segment(ObjectId addr);

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
