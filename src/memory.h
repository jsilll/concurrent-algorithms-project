#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <tm.h>
#include <stdatomic.h>

typedef _Atomic(tx_t) atomic_tx;

/// @brief Used for expressing the
/// status for a given segment in 
/// the transactional memory.
typedef enum _SegmentStatus
{
  /// @brief Used when segment
  /// has been removed.
  RM,
  /// @brief Used when segment
  /// has been allocated.
  ADDED,
  /// @brief Default segment status.
  DEFAULT,
  /// @brief Used when segment has
  /// been added after being removed.
  RM_N_ADDED,
} SegmentStatus;

/// @brief Used for expressing the
/// owner of a given segment in the
/// transactional memory.
typedef enum _SegmentOwner
{
  /// @brief Used when segment
  /// has no current owner
  NONE = 0,
  /// @brief Used when segment
  /// owner is a read only transaction.
  RO_TX = UINTPTR_MAX - 1,
  /// @brief Used when the segment
  /// is scheduled to be removed.
  RM_SCHED = UINTPTR_MAX - 2,
} SegmentOwner;

/// @brief Used for expressing
/// the region's batcher current status.
typedef enum _BatcherCounterStatus
{
  /// @brief Maximum number of threads
  /// the batcher can handle at each epoch
  N_TX = 12,
  /// @brief Used when there's multiple readers
  /// inside the batcher for the same epoch.
  M_RO_TX = UINTPTR_MAX - 1,
} BatcherCounterStatus;

/// @brief Represents a segment of memory in the STM.
typedef struct _Segment
{
  /// @brief Points to the actual data
  /// [v1, v2, controls].
  void *data;
  /// @brief Size of the data stored in 
  /// this segment (v1 and v2).
  size_t size;
  /// @brief Identifies the current 
  /// owner of the segment.
  atomic_tx owner;
  /// @brief Stores whether this segment 
  /// was added or removed in this epoch.
  atomic_int status;
} Segment;

/// @brief The goal of the Batcher is to artificially create 
/// points in time in which no transaction is running. The 
/// Batcher lets each and every blocked thread enter together 
/// when the last thread (from the previous epoch) leaves.
typedef struct _Batcher
{
  /// @brief Stores the which transaction
  /// should be making changes now.
  atomic_ulong turn;
  /// @brief Responsible for giving each
  /// transaction a unique identifier so
  /// that they know when its their turn.
  atomic_ulong take;
  /// @brief Stores the current batcher epoch.
  atomic_ulong epoch;
  /// @brief Number of transactions that 
  /// entered in the batcher in the current epoch.
  atomic_ulong n_tx_entered;
  /// @brief Number of transactions that still
  /// can enter in the batcher
  atomic_ulong remaining_slots;
  /// @brief Number of write transactions that
  /// entered in the batcher in the current epoch.
  atomic_ulong n_write_tx_entered;
} Batcher;

/// @brief Represents a region in the
/// software transactional memory
typedef struct _Region
{
  /// @brief User requested alignment 
  /// of the memory segments (bytes)
  size_t align;
  /// @brief Batcher for this memory region
  Batcher batcher;
  /// @brief Array of segments in this memory region
  Segment *segments;
  /// @brief True alignment of the memory 
  /// segments (bytes)
  size_t true_align;
  /// @brief Maximum index of any allocated
  /// memory segment in the region
  atomic_ulong index;
} Region;

#endif