#pragma once

#include <atomic>
#include <vector>
#include <cstdint>

#include "transaction.hpp"
#include "versioned_lock.hpp"

class Region
{
public:
  struct Word
  {
    std::uint64_t data; // TODO: change this to std::unique_ptr<char[]>
    VersionedLock lock;
  };

  struct Segment
  {
    std::size_t size;
    std::vector<Word> words;

    Segment(std::size_t size) : size(size), words(std::vector<Word>(size)) {}
  };

  static constexpr std::uintptr_t kFIRST = 1UL << 32;
  static constexpr std::uintptr_t kADDR_MASK = 0x000000000000FFFF;

public:
  std::atomic_uint gvc{0};
  std::atomic_uint32_t segs{1};

  std::size_t align;
  std::vector<Segment> mem;

  Region(std::size_t size, std::size_t align) : align(align) { mem.reserve(512); }

  inline Word &word(uintptr_t addr) noexcept
  {
    return mem[addr >> 32].words[(addr & kADDR_MASK) / align];
  }

  bool LockWriteSet(Transaction &transaction) noexcept
  {
    std::vector<std::reference_wrapper<Region::Word>> locked;
    locked.reserve(transaction.write_set.size());

    for (const auto &entry : transaction.write_set)
    {
      Region::Word &w = word(entry.first);
      if (!w.lock.TryLock(transaction.rv))
      {
        for (const auto l : locked)
        {
          l.get().lock.Unlock();
        }

        return false;
      }
      else
      {
        locked.push_back(w);
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
      if (ts.version > transaction.rv || ts.locked)
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
      Region::Word &wl = word(entry.first);
      memcpy(&wl.data, entry.second.get(), align);
      wl.lock.Unlock(transaction.wv);
    }
  }
};