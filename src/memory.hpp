#pragma once

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "segment.hpp"
#include "transaction.hpp"
#include "segment_manager.hpp"

class SharedMemory
{
public:
  SharedMemory(std::size_t size, std::size_t align) noexcept
      : align(align), allocator(size, align) {}

  ~SharedMemory() noexcept;

  SharedMemory(SharedMemory &&) = delete;
  SharedMemory &operator=(SharedMemory &&) = delete;

  SharedMemory(const SharedMemory &) = delete;
  SharedMemory &operator=(const SharedMemory &) = delete;

  Transaction* begin_tx(bool is_ro) noexcept;
  bool end_tx(Transaction &tx) noexcept;

  bool read_word(Transaction &tx, ObjectId src, char *dest) noexcept;
  bool write_word(Transaction &tx, const char *src, ObjectId dest) noexcept;

  bool allocate(Transaction &tx, std::size_t size, ObjectId *addr) noexcept;
  void free(Transaction &tx, ObjectId addr) noexcept;

  inline std::size_t size() const noexcept
  {
    return allocator.first_segment().size_bytes();
  };

  std::size_t alignment() const noexcept { return align; };

  inline ObjectId start_addr() const noexcept
  {
    return allocator.first_addr();
  }

private:
  void ref(TransactionDescriptor *desc);
  void unref(TransactionDescriptor *desc);

  void commit_frees(TransactionDescriptor &desc);

  void abort(Transaction &tx);
  void commit_changes(Transaction &tx);

  void read_word_readonly(const Transaction &tx, const Object &obj, char *dest) const noexcept;

  std::size_t align;
  SegmentAllocator allocator;
  std::atomic<TransactionDescriptor *> current{new TransactionDescriptor{0}};
  std::mutex descriptor_mutex;
};
