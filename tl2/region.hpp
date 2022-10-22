#pragma once

#include <atomic>
#include <vector>
#include <cstdint>

#include "transaction.hpp"
#include "version_lock.hpp"

class Region
{
public:
  struct Word
  {
    std::uint64_t word;
    VersionLock vlock;
  };

  struct Segment
  {
    std::size_t size;        // Size of the non-deallocable memory segment (in bytes)
    std::vector<Word> words; // Words of the segment
  };

  static constexpr uint64_t FIRST = 1UL << 32;

public:
  std::size_t align;             // Size of a word in the shared memory region (in bytes)
  std::vector<Segment> mem;      // Memory Segments
  std::atomic_uint global_vc{0}; // Global Version Clock
  std::atomic_uint64_t seg_cnt{2};

  Region(std::size_t size, std::size_t align)
      : align(align), mem(512, Segment{size, std::vector<Word>(1024)}) {}

  inline Word &word(uintptr_t addr)
  {
    return mem[addr >> 32].words[(addr & 0x0000FFFF) / align];
  }

  void release_lock_set(uint i, Transaction *transaction)
  {
    if (i == 0)
      return;
    for (const auto &target_src : transaction->write_set)
    {
      Region::Word &wl = word(target_src.first);
      wl.vlock.Release();
      if (i <= 1)
        break;
      i--;
    }
  }

  int try_acquire_sets(uint *i, Transaction *transaction)
  {
    *i = 0;
    for (const auto &target_src : transaction->write_set)
    {
      Region::Word &wl = word(target_src.first);
      bool acquired = wl.vlock.TryAcquire();
      if (!acquired)
      {
        release_lock_set(*i, transaction);
        return false;
      }
      *i = *i + 1;
    }
    return true;
  }

  bool validate_readset(Transaction *transaction)
  {
    for (const auto addr : transaction->read_set)
    {
      VersionLock::Value val = word(reinterpret_cast<uint64_t>(addr)).vlock.Sample();
      if ((val.locked) || val.version > transaction->rv)
      {
        return false;
      }
    }

    return true;
  }

  // release locks and update their version
  bool commit(Transaction *transaction)
  {
    for (const auto &target_src : transaction->write_set)
    {
      Region::Word &wl = word(target_src.first);
      memcpy(&wl.word, target_src.second.get(), align);
      wl.vlock.VersionedRelease(transaction->wv);
    }

    return true;
  }
};