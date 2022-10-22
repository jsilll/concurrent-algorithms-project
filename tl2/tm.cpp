#include <tm.hpp>

#include <map>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <iostream>
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
  // FIXME: write some nice abstraction for it
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

tx_t tm_begin(shared_t shared, bool is_ro) noexcept
{
  auto reg = static_cast<Region *>(shared);
  return reinterpret_cast<tx_t>(new Transaction{reg->global_vc.load(), is_ro});
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

  // for each word
  for (size_t i = 0; i < size / reg->align; i++)
  {
    uintptr_t word_addr = (uintptr_t)source + reg->align * i;
    Region::Word &word = reg->word(word_addr);                 // shared
    void *target_word = (void *)((uintptr_t)target + reg->align * i); // private

    if (!transaction->read_only)
    {
      auto it = transaction->write_set.find(word_addr); // O(logn)
      if (it != transaction->write_set.end())
      { // found in write-set
        memcpy(target_word, it->second.get(), reg->align);
        continue;
      }
    }

    VersionLock::Value prev_val = word.vlock.Sample();
    memcpy(target_word, &word.word, reg->align); // read word
    VersionLock::Value post_val = word.vlock.Sample();

    if (post_val.locked || (prev_val.version != post_val.version) || (prev_val.version > transaction->rv))
    {
      delete transaction;
      return false;
    }

    if (!transaction->read_only)
      transaction->read_set.emplace((void *)word_addr);
  }

  return true;
}

bool tm_end(shared_t shared, tx_t tx) noexcept
{
  auto reg = static_cast<Region *>(shared);
  auto transaction = reinterpret_cast<Transaction *>(tx);

  if (transaction->read_only || transaction->write_set.empty())
  {
    delete transaction;
    return true;
  }

  uint tmp;
  if (!reg->try_acquire_sets(&tmp, transaction))
  {
    delete transaction;
    return false;
  }

  transaction->wv = reg->global_vc.fetch_add(1) + 1;

  if ((transaction->rv != transaction->wv - 1) && !reg->validate_readset(transaction))
  {
    reg->release_lock_set(tmp, transaction);
    delete transaction;
    return false;
  }

  return reg->commit(transaction);
}

Alloc tm_alloc(shared_t shared, tx_t tx, size_t size, void **target) noexcept
{
  auto reg = static_cast<Region *>(shared);
  *target = (void *)(reg->seg_cnt.fetch_add(1) << 32);
  return Alloc::success;
}

bool tm_free(shared_t shared, tx_t tx, void *segment) noexcept
{
  // BIG FIXME!
  return true;
}
