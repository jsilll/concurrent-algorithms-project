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

  inline void read(char *dst, std::size_t size) const noexcept
  {
    std::memcpy(dst, buf.get(), size);
  }

  inline void write(const char *src, std::size_t size) const noexcept
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

  inline ObjectId() noexcept = default;

  inline ObjectId(std::uint8_t segment, std::size_t unused, std::size_t offset) noexcept
      : segment(segment), unused(unused), offset(offset)
  {
  }

  explicit inline ObjectId(const void *id) noexcept
  {
    static constexpr std::size_t OFFSET_MASK = (1ul << 55) - 1;
    static constexpr std::size_t SEGMENT_MASK = (1ul << 8) - 1;

    auto bytes = reinterpret_cast<std::size_t>(id);

    offset = bytes & OFFSET_MASK;
    segment = (bytes >> 56) & SEGMENT_MASK;
  }

  inline std::size_t to_opaque()
  {
    return (std::size_t(segment) << 56) | (1ul << 55) | offset;
  }

  inline friend ObjectId &operator+=(ObjectId &id, std::size_t offset) noexcept
  {
    id.offset += offset;
    return id;
  }

  inline friend ObjectId operator+(ObjectId id, std::size_t offset) noexcept
  {
    return id += offset;
  }

  inline friend bool operator==(const ObjectId &a, const ObjectId &b) noexcept
  {
    return a.segment == b.segment && a.offset == b.offset;
  }
};

/**
 * @brief Represents a shared segment in the STM.
 *
 */
class SharedSegment
{
private:
  std::size_t align_{1};
  std::size_t num_objects_{0};
  std::unique_ptr<Object[]> objects_{nullptr};
  std::atomic_flag should_delete_ = ATOMIC_FLAG_INIT;

  // ------------------ Constructors ----------------------- //

public:
  inline SharedSegment() = default;

  inline ~SharedSegment()
  {
    deallocate();
  }

  SharedSegment(const SharedSegment &) = delete;
  SharedSegment &operator=(const SharedSegment &) = delete;

  // ------------------ Getters ----------------------- //

public:
  inline Object &operator[](std::size_t idx) noexcept
  {
    return objects_[idx];
  }

  inline const Object &operator[](std::size_t idx) const noexcept
  {
    return objects_[idx];
  }

  inline std::size_t size() const noexcept
  {
    return num_objects_ * align_;
  }

  // ------------------ Public Methods ----------------------- //

public:
  inline bool mark_for_deletion()
  {
    return !should_delete_.test_and_set();
  }

  inline void cancel_deletion()
  {
    return should_delete_.clear();
  }

  void allocate(std::size_t size, std::size_t algn)
  {
    align_ = algn;
    num_objects_ = size / align_;
    objects_ = std::make_unique<Object[]>(num_objects_);
    for (auto i = 0ul; i < num_objects_; ++i)
    {
      objects_[i].latest.store(new ObjectVersion(align_), std::memory_order_relaxed);
    }
  }

  void deallocate()
  {
    for (auto i = 0ul; i < num_objects_; ++i)
    {
      auto version = objects_[i].latest.load();
      delete version;
    }
    objects_.reset();
    num_objects_ = 0;
    should_delete_.clear();
  }
};
