#pragma once

#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>

#include "versioned_lock.hpp"

struct ObjectVersion
{
  ObjectVersion(std::size_t size) : buf(std::make_unique<char[]>(size))
  {
    std::memset(buf.get(), 0, size);
  }

  ObjectVersion(std::unique_ptr<char[]> buf) : buf(std::move(buf)) {}

  std::unique_ptr<char[]> buf;
  VersionedLock::Timestamp version = 0;
  ObjectVersion *earlier = nullptr;

  void read(char *dst, std::size_t size) const noexcept
  {
    std::memcpy(dst, buf.get(), size);
  }

  void write(const char *src, std::size_t size) const noexcept
  {
    std::memcpy(buf.get(), src, size);
  }
};

struct Object
{
  VersionedLock lock;
  std::atomic<ObjectVersion *> latest{nullptr};
};

struct ObjectId
{
  std::uint8_t segment : 8;
  std::size_t unused : 1;
  std::size_t offset : 55;

  inline std::size_t to_opaque()
  {
    return (std::size_t(segment) << 56) | (1ul << 55) | offset;
  }
};

inline ObjectId &operator+=(ObjectId &id, std::size_t offset) noexcept
{
  id.offset += offset;
  return id;
}

inline ObjectId operator+(ObjectId id, std::size_t offset) noexcept
{
  return id += offset;
}

inline ObjectId to_object_id(const void *id) noexcept
{
  static constexpr std::size_t OFFSET_MASK = (1ul << 55) - 1;
  static constexpr std::size_t SEGMENT_MASK = (1ul << 8) - 1;

  auto bytes = reinterpret_cast<std::size_t>(id);
  std::size_t offset = bytes & OFFSET_MASK;
  std::uint8_t segment = (bytes >> 56) & SEGMENT_MASK;

  return ObjectId{segment, 1, offset};
}

inline bool operator==(const ObjectId &a, const ObjectId &b) noexcept
{
  return a.segment == b.segment && a.offset == b.offset;
}

class SharedSegment
{
public:
  SharedSegment() = default;

  SharedSegment(const SharedSegment &) = delete;
  SharedSegment &operator=(const SharedSegment &) = delete;

  ~SharedSegment() { deallocate(); }

  void allocate(std::size_t size, std::size_t algn)
  {
    align = algn;
    num_objects = size / align;
    objects = std::make_unique<Object[]>(num_objects);
    for (auto i = 0ul; i < num_objects; ++i)
    {
      objects[i].latest.store(new ObjectVersion(align),
                              std::memory_order_relaxed);
    }
  }

  void deallocate()
  {
    for (auto i = 0ul; i < num_objects; ++i)
    {
      auto version = objects[i].latest.load();
      delete version;
    }
    objects.reset();
    num_objects = 0;
    should_delete.clear();
  }

  // Returns true if marking succeeded
  bool mark_for_deletion() { return !should_delete.test_and_set(); }

  void cancel_deletion() { return should_delete.clear(); }

  [[nodiscard]] Object &operator[](std::size_t idx) noexcept
  {
    return objects[idx];
  }

  [[nodiscard]] const Object &operator[](std::size_t idx) const noexcept
  {
    return objects[idx];
  }

  [[nodiscard]] std::size_t size_bytes() const noexcept
  {
    return num_objects * align;
  }

private:
  std::atomic_flag should_delete = ATOMIC_FLAG_INIT;
  std::size_t num_objects = 0, align = 1;
  std::unique_ptr<Object[]> objects = nullptr;
};
