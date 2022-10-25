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
    std::size_t size;                                  
    std::vector<Word> words = std::vector<Word>(1024); 
  };

  static constexpr uint64_t FIRST = 1UL << 32;

public:
  std::size_t align;        
  std::vector<Segment> mem; 
  std::atomic_uint gvc{0};  
  std::atomic_uint64_t segs{2};

  Region(std::size_t size, std::size_t align)
      : align(align), mem(512, Segment{size}) {}

  inline Word &word(uintptr_t addr)
  {
    return mem[addr >> 32].words[(addr & 0x0000FFFF) / align];
  }

  void UnlockWriteSet(Transaction *transaction) noexcept
  {
    for (const auto &target_src : transaction->write_set) 
    {
      word(target_src.first).vlock.Release();
    }
  }

  bool LockWriteSet(Transaction *transaction) noexcept
  {
    int locked = 0;
    for (const auto &target_src : transaction->write_set)
    {
      if (!word(target_src.first).vlock.TryAcquire())
      {
        UnlockWriteSet(locked, transaction);
        return false;
      }
      ++locked;
    }

    return true;
  }

  bool ValidateReadSet(Transaction *transaction) noexcept
  {
    for (const auto addr : transaction->read_set)
    {
      VersionLock::Value val = word(reinterpret_cast<uint64_t>(addr)).vlock.Sample();
      if (val.locked || val.version > transaction->rv) 
      {
        return false;
      }
    }

    return true;
  }

  void Commit(Transaction *transaction) noexcept
  {
    for (const auto &target_src : transaction->write_set)
    {
      Region::Word &wl = word(target_src.first);
      memcpy(&wl.word, target_src.second.get(), align);
      wl.vlock.VersionedRelease(transaction->wv);
    }
  }

private:
  void UnlockWriteSet(int locked, Transaction *transaction) noexcept
  {
    if (locked == 0) 
    {
      return;
    }

    for (const auto &target_src : transaction->write_set)
    {
      locked--;
      word(target_src.first).vlock.Release();
      if (locked == 0) break;
    }

  }
};