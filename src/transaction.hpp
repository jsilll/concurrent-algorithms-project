#pragma once

#include <atomic>
#include <vector>

#include "segment.hpp"

struct TransactionDescriptor
{
  VersionedLock::Timestamp commit_time = 0;
  std::atomic_uint_fast32_t refcount{1};
  std::vector<std::unique_ptr<ObjectVersion>> objects_to_delete{};
  std::vector<ObjectId> segments_to_delete{};

  TransactionDescriptor *next = nullptr;
};

struct Transaction
{
  struct WriteEntry
  {
    ObjectId addr;
    Object &obj;
    std::unique_ptr<char[]> written;
  };

  struct ReadEntry
  {
    ObjectId addr;
    Object &obj;
  };

  [[nodiscard]] WriteEntry *find_write_entry(ObjectId addr) noexcept
  {
    for (auto &entry : write_set)
    {
      if (entry.addr == addr)
      {
        return &entry;
      }
    }
    return nullptr;
  }

  bool is_ro;
  TransactionDescriptor *start_point;
  VersionedLock::Timestamp start_time;
  std::vector<WriteEntry> write_set;
  std::vector<ReadEntry> read_set;
  std::vector<ObjectId> alloc_set;
  std::vector<ObjectId> free_set;
};
