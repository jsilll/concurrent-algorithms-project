#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <tm.h>
#include <unistd.h>

#include "macros.h"
#include "relinquish_cpu.h"

// TODO: figure this out, refactor and change names
#define BATCHER_NB_TX 12  // looks like maximum number of transactions allowed
#define MULTIPLE_READERS UINTPTR_MAX - BATCHER_NB_TX

// TODO: change names
typedef enum {
  /// @brief
  NO_OWNER = 0,
  /// @brief
  READ_ONLY_TX = UINTPTR_MAX - 1,
  /// @brief
  REMOVE_SCHEDULED = UINTPTR_MAX - 2,
} TransactionStatus;

// TODO: change names and add comments
typedef enum {
  /// @brief Represents when a
  ADDED,
  /// @brief
  DEFAULT_FLAG,
  /// @brief
  REMOVED,
  /// @brief
  REMOVED_AFTER_ADD,
} SegmentStatus;

typedef _Atomic(tx_t) atomic_tx;

/// @brief Represents a segment of memory in the STM
typedef struct {
  /// @brief Points to the actual data [control, mem1, mem2]
  void *data;
  /// @brief Size of the data stored in this segment
  size_t size;
  /// @brief Stores whether this segment needs to be added / removed  (for
  /// rollback and commit)
  atomic_int status;
  /// @brief Identifies the current owner of the segment
  atomic_tx owner;
} Segment;

typedef struct {
  /// @brief turn
  atomic_ulong turn;
  /// @brief take?
  atomic_ulong take;
  /// @brief epoch
  atomic_ulong epoch;
  /// @brief number of threads in the Batcher?
  atomic_ulong counter;
  /// @brief number of transactions that entered in the batcher
  atomic_ulong n_tx_entered;
  /// @brief number of write transactions entered
  atomic_ulong n_write_tx_entered;
} Batcher;

typedef struct {
  /// @brief Claimed alignment of the shared memory region (in bytes)
  size_t align;
  /// @brief
  Batcher batcher;
  /// @brief
  Segment *segment;
  /// @brief Actual alignment of the memory allocations (in bytes)
  size_t align_alloc;
  /// @brief
  atomic_ulong index;
} Region;

static inline tx_t Enter(Region *region, bool is_ro) {
  if (is_ro) {
    // Waiting for our turn
    unsigned long int turn = atomic_fetch_add(&(region->batcher.take), 1);
    while (turn != atomic_load(&(region->batcher.turn))) {
      relinquish_cpu();
    }
    // Incrementing number of transactions entered in batcher and giving turn
    atomic_fetch_add(&(region->batcher.n_tx_entered), 1);
    atomic_fetch_add(&(region->batcher.turn), 1);
    return READ_ONLY_TX;
  }

  while (true) {
    // Waiting for our turn
    unsigned long int turn = atomic_fetch_add(&(region->batcher.take), 1);
    while (turn != atomic_load(&(region->batcher.turn))) {
      relinquish_cpu();
    }
    // We can proceed
    if (atomic_load(&(region->batcher.counter)) != 0) {
      atomic_fetch_add(&(region->batcher.counter), -1);
      break;
    }
    // TODO: apparently this order works
    // Giving away turn
    atomic_fetch_add(&(region->batcher.turn), 1);
    // Waiting for next epoch
    unsigned long int last_epoch = atomic_load(&(region->batcher.epoch));
    while (last_epoch == atomic_load(&(region->batcher.epoch))) {
      relinquish_cpu();
    }
  }
  // Incrementing number of transactions entered,
  // write transactions entered and giving turn
  tx_t tx = atomic_fetch_add(&(region->batcher.n_write_tx_entered), 1) + 1;
  atomic_fetch_add(&(region->batcher.n_tx_entered), 1);
  atomic_fetch_add(&(region->batcher.turn), 1);
  return tx;
}

static inline bool Leave(Region *region, tx_t tx) {
  // Waiting for our turn
  unsigned long int turn = atomic_fetch_add(&(region->batcher.take), 1);
  while (turn != atomic_load(&(region->batcher.turn))) {
    relinquish_cpu();
  }
  // Check if this is the last write transaction
  if (atomic_fetch_add(&region->batcher.n_tx_entered, -1) == 1 && atomic_load(&(region->batcher.n_write_tx_entered))) {
    // Write transaction
    for (size_t i = region->index - 1; i < region->index; --i) {
      Segment *segment = region->segment + i;
      // If this segment is meant to be deleted
      if (segment->owner == REMOVE_SCHEDULED || segment->status == REMOVED || segment->status == REMOVED_AFTER_ADD) {
        unsigned long int expected_index = i + 1;
        if (!atomic_compare_exchange_strong(&(region->index), &expected_index, i)) {
          // Resetting owner and status flags
          segment->status = DEFAULT_FLAG;
          segment->owner = REMOVE_SCHEDULED;
        } else {
          // Freeing allocated space
          free(segment->data);
          segment->data = NULL;
        }
      } else {
        // Commiting writes
        memcpy(segment->data, (char *)(segment->data) + segment->size, segment->size);
        // Reseting all the locks
        bzero((char *)(segment->data) + (segment->size << 1), (segment->size / region->align) * sizeof(tx_t));
      }
      // Resetting owner and status flags
      segment->owner = NO_OWNER;
      segment->status = DEFAULT_FLAG;
    }
    // Resetting counter, n_write_tx_entered
    atomic_store(&(region->batcher.counter), BATCHER_NB_TX);
    atomic_store(&(region->batcher.n_write_tx_entered), 0);
    // Moving to next epoch
    atomic_fetch_add(&(region->batcher.epoch), 1);
  } else if (tx != READ_ONLY_TX) {
    // Giving away turn
    atomic_fetch_add(&(region->batcher.turn), 1);
    // Waiting for the next epoch
    unsigned long int epoch = atomic_load(&(region->batcher.epoch));
    while (epoch == atomic_load(&(region->batcher.epoch))) {
      relinquish_cpu();
    }
    return true;
  }
  // Giving away turn
  atomic_fetch_add(&(region->batcher.turn), 1);
  return true;
}

static inline Segment *LookupSegment(const Region *region, const void *source) {
  // For each segment in region
  for (size_t i = region->index - 1; i < region->index; --i) {
    // Segment has been deleted
    if (region->segment[i].owner == REMOVE_SCHEDULED) {
      return NULL;
    }
    // Check if source is contained within segment's range
    if ((char *)source >= (char *)region->segment[i].data && (char *)source < (char *)region->segment[i].data + region->segment[i].size) {
      return region->segment + i;
    }
  }
  return NULL;
}

bool Lock(Region *region, Segment *segment, tx_t tx, void *target, size_t size) {
  size_t base_index = ((char *)target - (char *)segment->data) / region->align;
  // Getting the beggining of the controls words
  atomic_tx *controls = (atomic_tx *)((char *)segment->data + (segment->size << 1)) + base_index;
  // For each requested word
  for (size_t i = 0; i < size / region->align; ++i) {
    tx_t previous = 0, previously_read = -tx;
    if (!(atomic_compare_exchange_strong(controls + i, &previous, tx) || previous == tx || atomic_compare_exchange_strong(controls + i, &previously_read, tx))) {
      if (i > 1) {
        bzero(controls, (i - 1) * sizeof(tx_t));
      }
      return false;
    }
  }
  return true;
}

static inline void Undo(Region *region, tx_t tx) {
  // For each segment in region
  for (size_t i = region->index - 1; i < region->index; --i) {
    Segment *segment = region->segment + i;
    // Undo malloc of new segment
    if ((segment->status == ADDED || segment->status == REMOVED_AFTER_ADD) && tx == segment->owner) {
      segment->owner = REMOVE_SCHEDULED;
    } else if (segment->data != NULL && segment->owner != REMOVE_SCHEDULED) {
      // Reset segment in case its ours
      if (segment->owner == tx) {
        segment->owner = NO_OWNER;
        segment->status = DEFAULT_FLAG;
      }
      // Control words
      atomic_tx *controls = (atomic_tx *)((char *)segment->data + (segment->size << 1));
      // For each word in the segment
      for (size_t j = 0; j < segment->size / region->align; ++j) {
        // If we are the owner
        if (atomic_load(controls + j) == tx) {
          memcpy((char *)segment->data + segment->size + j * region->align, (char *)segment->data + j * region->align, region->align);
          atomic_store(controls + j, NO_OWNER);
        } else {
          tx_t previously_read = -tx;
          atomic_compare_exchange_weak(controls + j, &previously_read, NO_OWNER);
        }
      }
    }
  }
  // Leaving transaction
  Leave(region, tx);
}

#endif