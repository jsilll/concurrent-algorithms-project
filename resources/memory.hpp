#pragma once

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "segment.hpp"
#include "transaction.hpp"
#include "segment_manager.hpp"

/**
 * @brief Represents a region of shared memory in the STM.
 *
 */
class SharedMemory
{
private:
  std::size_t align_;
  SegmentManager allocator_;

  std::mutex descriptor_mutex_;
  std::atomic<TransactionDescriptor *> current_{new TransactionDescriptor{0}};

public:
  inline SharedMemory(std::size_t size, std::size_t align) noexcept
      : align_(align), allocator_(size, align) {}

  inline ~SharedMemory() noexcept
  {
    unref(current_.load());
  }

  SharedMemory(SharedMemory &&) = delete;
  SharedMemory &operator=(SharedMemory &&) = delete;

  SharedMemory(const SharedMemory &) = delete;
  SharedMemory &operator=(const SharedMemory &) = delete;

public:
  inline std::size_t alignment() const noexcept
  {
    return align_;
  }

  inline std::size_t size() const noexcept
  {
    return allocator_.first().size();
  }

  inline ObjectId start_addr() const noexcept
  {
    return allocator_.start();
  }

public:
  bool end_tx(Transaction &tx) noexcept
  {
    if (tx.is_ro)
    {
      unref(tx.start_point);
      return true;
    }

    // First, try acquiring all locks in the write set
    auto it = tx.write_set.begin();
    auto rollback_locks = [&it, &tx]
    {
      unlock_all(tx.write_set.begin(), it);
    };

    std::unordered_set<std::size_t> acquired_locks;
    while (it != tx.write_set.end())
    {
      if (!it->obj.lock.try_lock(tx.start_time))
      {
        rollback_locks();
        abort(tx);
        return false;
      }
      acquired_locks.insert(it->addr.to_opaque());
      it++;
    }

    // Validate read set
    for (auto &read : tx.read_set)
    {
      if (acquired_locks.find(read.addr.to_opaque()) != acquired_locks.end())
      {
        continue;
      }
      if (!read.obj.lock.validate(tx.start_time))
      {
        rollback_locks();
        abort(tx);
        return false;
      }
    }

    {
      std::unique_lock<std::mutex> lock(descriptor_mutex_);
      commit_changes(tx);
    }

    return true;
  }

  Transaction *begin_tx(bool is_ro) noexcept
  {
    auto tx = new Transaction;
    tx->is_ro = is_ro;

    TransactionDescriptor *start_point;

    {
      std::unique_lock<std::mutex> lock(descriptor_mutex_);
      start_point = current_.load(std::memory_order_acquire);
      ref(start_point);
    }

    tx->start_time = start_point->commit_time;
    tx->start_point = start_point;
    return tx;
  }

  bool read_word(Transaction &tx, ObjectId src, char *dst) noexcept
  {
    auto &obj = allocator_.find(src);
    if (tx.is_ro)
    {
      read_word_readonly(tx, obj, dst);
      return true;
    }

    if (auto entry = tx.find_write_entry(src))
    {
      std::memcpy(dst, entry->written.get(), align_);
      return true;
    }

    auto latest = obj.latest.load(std::memory_order_acquire);
    if (!obj.lock.validate(tx.start_time))
    {
      abort(tx);
      return false;
    }
    tx.read_set.push_back({src, obj});
    latest->read(dst, align_);
    return true;
  }

  bool write_word(Transaction &tx, const char *src, ObjectId dst) noexcept
  {
    if (auto entry = tx.find_write_entry(dst))
    {
      std::memcpy(entry->written.get(), src, align_);
      return true;
    }

    auto &obj = allocator_.find(dst);
    auto written = clone(src, align_);
    tx.write_set.push_back({dst, obj, std::move(written)});
    return true;
  }

  void free(Transaction &tx, ObjectId addr) noexcept
  {
    if (allocator_.find_segment(addr).mark_for_deletion())
    {
      tx.free_set.push_back(addr);
    }
  }

  bool allocate(Transaction &tx, std::size_t size, ObjectId *dest) noexcept
  {
    auto success = allocator_.allocate(size, dest);
    if (success)
    {
      tx.alloc_set.push_back(*dest);
    }
    return success;
  }

private:
  static void unlock_all(std::vector<Transaction::WriteEntry>::iterator begin, std::vector<Transaction::WriteEntry>::iterator end)
  {
    while (begin != end)
    {
      begin->obj.lock.unlock();
      begin++;
    }
  }

  static std::unique_ptr<char[]> clone(const char *word, std::size_t align)
  {
    auto copy = std::make_unique<char[]>(align);
    std::memcpy(copy.get(), word, align);
    return copy;
  }

private:
  void read_word_readonly(const Transaction &tx, const Object &obj, char *dst) const noexcept
  {
    auto ver = obj.latest.load(std::memory_order_acquire);
    while (ver->version > tx.start_time)
    {
      ver = ver->earlier;
    }
    ver->read(dst, align_);
  }

  void ref(TransactionDescriptor *desc) noexcept
  {
    if (desc == nullptr)
    {
      return;
    }
    desc->refcount.fetch_add(1, std::memory_order_acq_rel);
  }

  void unref(TransactionDescriptor *desc) noexcept
  {
    if (desc == nullptr)
    {
      return;
    }

    auto previous = desc->refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1)
    {
      unref(desc->next);
      commit_frees(*desc);
      delete desc;
    }
  }

  void abort(Transaction &tx) noexcept
  {
    for (auto segment : tx.alloc_set)
    {
      allocator_.free(segment);
    }
    for (auto segment : tx.free_set)
    {
      allocator_.find_segment(segment).cancel_deletion();
    }
    unref(tx.start_point);
  }

  void commit_changes(Transaction &tx) noexcept
  {
    auto cur_point = current_.load(std::memory_order_acquire);

    auto commit_time = cur_point->commit_time + 1;
    auto *descr = new TransactionDescriptor{commit_time};

    cur_point->next = descr;
    ref(descr);

    unref(cur_point);

    current_.store(descr, std::memory_order_release);

    descr->segments_to_delete = std::move(tx.free_set);

    for (auto &write : tx.write_set)
    {
      auto &obj = write.obj;

      auto *old_version = obj.latest.load(std::memory_order_acquire);

      auto *new_version = new ObjectVersion(std::move(write.written));

      new_version->version = commit_time;
      new_version->earlier = old_version;

      obj.latest.store(new_version, std::memory_order_release);
      descr->objects_to_delete.emplace_back(old_version);
      obj.lock.unlock(commit_time);
    }

    unref(tx.start_point);
  }

  void commit_frees(TransactionDescriptor &desc) noexcept
  {
    for (auto segm : desc.segments_to_delete)
    {
      allocator_.free(segm);
    }
  }
};
