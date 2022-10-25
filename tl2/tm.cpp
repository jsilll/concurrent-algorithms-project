#include <tm.hpp>

#include <map>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <string.h>
#include <shared_mutex>
#include <unordered_set>

#ifdef DEBUG
#include <iostream>
#endif

#include "expect.hpp"
#include "region.hpp"
#include "transaction.hpp"
#include "versioned_lock.hpp"

shared_t tm_create(size_t size, size_t align) noexcept
{
#ifdef DEBUG
  std::cout << "tm_create()" << std::endl;
#endif
  try
  {
    return static_cast<void *>(new Region(size, align));
  }
  catch (const std::exception &e)
  {
    return invalid_shared;
  }
}

void tm_destroy(shared_t shared) noexcept
{
#ifdef DEBUG
  std::cout << "tm_destroy()" << std::endl;
#endif
  delete static_cast<Region *>(shared);
}

void *tm_start(shared_t shared) noexcept
{
#ifdef DEBUG
  std::cout << "tm_start()" << std::endl;
#endif
  return reinterpret_cast<void *>(Region::kFIRST);
}

size_t tm_size(shared_t shared) noexcept
{
#ifdef DEBUG
  std::cout << "tm_size()" << std::endl;
#endif
  return static_cast<Region *>(shared)->mem[0].size;
}

size_t tm_align(shared_t shared) noexcept
{
#ifdef DEBUG
  std::cout << "tm_align()" << std::endl;
#endif
  return static_cast<Region *>(shared)->align;
}

tx_t tm_begin(shared_t shared, bool ro) noexcept
{
#ifdef DEBUG
  std::cout << "tm_begin()" << std::endl;
#endif
  auto region = static_cast<Region *>(shared);
  return reinterpret_cast<tx_t>(new Transaction{region->gvc.load(), ro});
}

bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target) noexcept
{
#ifdef DEBUG
  std::cout << "tm_write()" << std::endl;
#endif
  auto region = static_cast<Region *>(shared);
  auto transaction = reinterpret_cast<Transaction *>(tx);

  auto source_word_base = reinterpret_cast<uintptr_t>(source);
  auto target_word_base = reinterpret_cast<uintptr_t>(target);

  for (size_t offset = 0; offset < size; offset += region->align)
  {
    uintptr_t target_word = target_word_base + offset;
    transaction->write_set[target_word] = std::make_unique<char[]>(region->align);
    auto source_word = reinterpret_cast<void *>(source_word_base + offset);
    memcpy(transaction->write_set[target_word].get(), source_word, region->align);
  }

#ifdef DEBUG
  std::cout << "tm_write() finished" << std::endl;
#endif
  return true;
}

bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target) noexcept
{
#ifdef DEBUG
  std::cout << "tm_read()" << std::endl;
#endif
  auto region = static_cast<Region *>(shared);
  auto transaction = reinterpret_cast<Transaction *>(tx);

  if (transaction->ro)
  {
    for (size_t offset = 0; offset < size; offset += region->align)
    {
      uintptr_t addr = reinterpret_cast<uintptr_t>(source) + offset;

      Region::Word &word = region->word(addr);
      void *target_addr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(target) + offset);

      VersionedLock::TimeStamp ts = word.lock.Sample();
      if (ts.locked or transaction->rv < ts.version)
      {
        delete transaction;
#ifdef DEBUG
        std::cout << "tm_read() failed on write transaction." << std::endl;
#endif
        return false;
      }
      else
      {
        memcpy(target_addr, &word.word, region->align);
      }
    }
  }
  else
  {
    for (size_t offset = 0; offset < size; offset += region->align)
    {
      uintptr_t addr = reinterpret_cast<uintptr_t>(source) + offset;
      transaction->read_set.emplace(addr);

      Region::Word &word = region->word(addr);
      void *target_addr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(target) + offset);

      auto entry = transaction->write_set.find(addr);
      if (entry != transaction->write_set.end())
      {
        memcpy(target_addr, entry->second.get(), region->align);
      }
      else
      {
        VersionedLock::TimeStamp ts = word.lock.Sample();
        if (ts.locked or transaction->rv < ts.version)
        {
          delete transaction;
#ifdef DEBUG
          std::cout << "tm_read() failed on read transaction." << std::endl;
#endif
          return false;
        }
        else
        {
          memcpy(target_addr, &word.word, region->align);
        }
      }
    }
  }

#ifdef DEBUG
  std::cout << "tm_read() completed." << std::endl;
#endif
  return true;
}

bool tm_end(shared_t shared, tx_t tx) noexcept
{
  auto region = static_cast<Region *>(shared);
  auto transaction = reinterpret_cast<Transaction *>(tx);

  if (transaction->ro)
  {
    delete transaction;
    return true;
  }

  if (!region->LockWriteSet(*transaction))
  {
    delete transaction;
    return false;
  }

  transaction->wv = region->gvc.fetch_add(1) + 1;

  if (transaction->rv + 1 == transaction->wv)
  {
    region->Commit(*transaction);
    delete transaction;
    return true;
  }

  if (!region->ValidateReadSet(*transaction))
  {
    region->UnlockWriteSet(*transaction);
    delete transaction;
    return false;
  }

  region->Commit(*transaction);
  delete transaction;
  return true;
}

Alloc tm_alloc(shared_t shared, tx_t tx, size_t size, void **target) noexcept
{
#ifdef DEBUG
  std::cout << "tm_alloc()" << std::endl;
#endif
  auto region = static_cast<Region *>(shared);
  *target = reinterpret_cast<void *>(region->segs.fetch_add(1) << 32);
  return Alloc::success;
}

bool tm_free(shared_t shared, tx_t tx, void *segment) noexcept
{
#ifdef DEBUG
  std::cout << "tm_free()" << std::endl;
#endif
  return true;
}
