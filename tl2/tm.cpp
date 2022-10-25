#include <tm.hpp>

#include <map>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <string.h>
#include <shared_mutex>
#include <unordered_set>

#include "expect.hpp"
#include "region.hpp"
#include "transaction.hpp"
#include "version_lock.hpp"

shared_t tm_create(size_t size, size_t align) noexcept
{
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
  delete static_cast<Region *>(shared);
}

void *tm_start(shared_t shared) noexcept
{
  return reinterpret_cast<void *>(Region::FIRST);
}

size_t tm_size(shared_t shared) noexcept
{
  return static_cast<Region *>(shared)->mem[0].size;
}

size_t tm_align(shared_t shared) noexcept
{
  return static_cast<Region *>(shared)->align;
}

tx_t tm_begin(shared_t shared, bool ro) noexcept
{
  auto reg = static_cast<Region *>(shared);
  return reinterpret_cast<tx_t>(new Transaction{reg->gvc.load(), ro});
}

bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target) noexcept
{
  auto reg = static_cast<Region *>(shared);
  auto transaction = reinterpret_cast<Transaction *>(tx);

  auto source_word_base = reinterpret_cast<uintptr_t>(source);
  auto target_word_base = reinterpret_cast<uintptr_t>(target);

  for (size_t offset = 0; offset < size; offset += reg->align)
  {
    uintptr_t target_word = target_word_base + offset;
    transaction->write_set[target_word] = std::make_unique<char[]>(reg->align);
    auto source_word = reinterpret_cast<void *>(source_word_base + offset);
    memcpy(transaction->write_set[target_word].get(), source_word, reg->align);
  }

  return true;
}

bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target) noexcept
{
  auto reg = static_cast<Region *>(shared);
  auto transaction = reinterpret_cast<Transaction *>(tx);

  if (transaction->ro)
  {
    for (size_t offset = 0; offset < size; offset += reg->align)
    {
      uintptr_t addr = reinterpret_cast<uintptr_t>(source) + offset;

      Region::Word &word = reg->word(addr);
      void *target_addr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(target) + offset);

      VersionLock::Value lockval = word.vlock.Sample();
      if (lockval.locked or transaction->rv < lockval.version)
      {
        delete transaction;
        return false;
      }
      else
      {
        memcpy(target_addr, &word.word, reg->align);
      }
    }
  }
  else
  {
    for (size_t offset = 0; offset < size; offset += reg->align)
    {
      uintptr_t addr = reinterpret_cast<uintptr_t>(source) + offset;
      transaction->read_set.emplace(reinterpret_cast<void *>(addr));

      Region::Word &word = reg->word(addr);
      void *target_addr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(target) + offset);

      auto entry = transaction->write_set.find(addr);
      if (entry != transaction->write_set.end())
      {
        memcpy(target_addr, entry->second.get(), reg->align);
      }
      else
      {
        VersionLock::Value lockval = word.vlock.Sample();
        if (lockval.locked or transaction->rv < lockval.version)
        {
          delete transaction;
          return false;
        }
        else
        {
          memcpy(target_addr, &word.word, reg->align);
        }
      }
    }
  }

  return true;
}

bool tm_end(shared_t shared, tx_t tx) noexcept
{
  auto reg = static_cast<Region *>(shared);
  auto transaction = reinterpret_cast<Transaction *>(tx);

  if (transaction->ro)
  {
    delete transaction;
    return true;
  }

  if (!reg->LockWriteSet(transaction))
  {
    delete transaction;
    return false;
  }

  transaction->wv = reg->gvc.fetch_add(1) + 1;

  if (transaction->rv + 1 == transaction->wv)
  {
    reg->UnlockWriteSet(transaction);
    delete transaction;
    return false;
  }

  if (!reg->ValidateReadSet(transaction))
  {
    reg->UnlockWriteSet(transaction);
    delete transaction;
    return false;
  }

  reg->Commit(transaction);
  return true; 
}

Alloc tm_alloc(shared_t shared, tx_t tx, size_t size, void **target) noexcept
{
  auto reg = static_cast<Region *>(shared);
  *target = (void *)(reg->segs.fetch_add(1) << 32);
  return Alloc::success;
}

bool tm_free(shared_t shared, tx_t tx, void *segment) noexcept
{
  return true;
}
