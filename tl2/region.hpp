#pragma once

#include <mutex>
#include <atomic>
#include <vector>
#include <cstdint>
#include <iostream>

#include "transaction.hpp"
#include "versioned_lock.hpp"

class Region
{
public:
  struct Word
  {
    // TODO: change this to std::unique_ptr<char[]>
    std::uint64_t data;
    VersionedLock lock;
  };

  struct Segment
  {
    std::size_t size;
    std::vector<Word> words;

    Segment(std::size_t size) noexcept : size(size), words(size) {}
  };

public:
  static constexpr std::uint64_t FIRST{0};

public:
  std::atomic_uint32_t gvc{0};
  std::atomic_uint32_t segs{1};
  
  std::size_t align;
  std::mutex mem_mutex;
  std::vector<Segment> mem;

  Region(std::size_t size, std::size_t align) noexcept : align(align)
  {
    // mem.reserve(512);
    mem.emplace_back(size);
  }

  inline Word &word(std::uintptr_t addr) noexcept
  {
    return mem[addr >> 32].words[(addr & 0x0000FFFF) / align];
  }

  inline std::uintptr_t Alloc(std::size_t size)
  {
    mem.emplace_back(size);
    return static_cast<std::uintptr_t>(segs.fetch_add(1)) << 32;
  }

  bool LockWriteSet(Transaction &transaction) noexcept
  {
    std::vector<std::reference_wrapper<Region::Word>> locked;
    locked.reserve(transaction.write_set.size());

    for (const auto &entry : transaction.write_set)
    {
      Region::Word &w = word(entry.first);
      if (w.lock.TryLock(transaction.rv))
      {
        locked.push_back(w);
      }
      else
      {
        for (const auto l : locked)
        {
          l.get().lock.Unlock();
        }

        return false;
      }
    }

    return true;
  }

  void UnlockWriteSet(Transaction &transaction) noexcept
  {
    for (const auto &entry : transaction.write_set)
    {
      word(entry.first).lock.Unlock();
    }
  }

  bool ValidateReadSet(Transaction &transaction) noexcept
  {
    for (const auto addr : transaction.read_set)
    {
      VersionedLock::TimeStamp ts = word(addr).lock.Sample();
      if (ts.locked || ts.version > transaction.rv)
      {
        return false;
      }
    }

    return true;
  }

  void Commit(Transaction &transaction) noexcept
  {
    for (const auto &entry : transaction.write_set)
    {
      Region::Word &w = word(entry.first);
      memcpy(&w.data, entry.second.get(), align);
      w.lock.Unlock(transaction.wv);
    }
  }
};