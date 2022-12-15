#pragma once

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yield.h"
#include "macros.h"
#include "internal_memory.h"

static inline void LockBatcher(Batcher *batcher)
{
  spin_lock_acquire(&(batcher->lock));
}

static inline void UnlockBatcher(Batcher *batcher)
{
  spin_lock_release(&(batcher->lock));
}

static inline void WaitForNextBatcherEpoch(Batcher *batcher)
{
  unsigned long int last = atomic_load(&(batcher->counter));
  while (last == atomic_load(&(batcher->counter)))
  {
    yield();
  }
}

static inline tx_t Enter(Region *region, bool is_ro)
{
  if (is_ro)
  {
    LockBatcher(&(region->batcher));

    // Incrementing number of transactions that entered in batcher
    ++region->batcher.n_entered;

    UnlockBatcher(&(region->batcher));

    return RO_TX;
  }

  while (true)
  {
    LockBatcher(&(region->batcher));

    if (region->batcher.n_write_slots != 0)
    {
      // We can proceed
      --region->batcher.n_write_slots;
      break;
    }

    UnlockBatcher(&(region->batcher));

    WaitForNextBatcherEpoch(&(region->batcher));
  }

  // Incrementing number of write transactions that entered
  tx_t tx = WRITE_TX_PER_EPOCH_MAX - region->batcher.n_write_slots;

  // Incrementing number of transactions entered,
  ++region->batcher.n_entered;

  UnlockBatcher(&(region->batcher));

  return tx;
}

static inline bool Leave(Region *region, tx_t tx)
{
  LockBatcher(&(region->batcher));

  // Check if this is the last transaction
  if (region->batcher.n_entered-- == 1 && region->batcher.n_write_slots < WRITE_TX_PER_EPOCH_MAX)
  {
    // Write transaction
    for (size_t i = region->index - 1; i < region->index; --i)
    {
      Segment *segment = region->segments + i;
      // If this segment is meant to be deleted
      if (atomic_load(&(segment->owner)) == RM_TX || atomic_load(&(segment->status)) == REMOVED || atomic_load(&(segment->status)) == ADDED_AFTER_REMOVE)
      {
        unsigned long int expected = i + 1;
        if (!atomic_compare_exchange_strong(&(region->index), &expected, i))
        {
          // Resetting owner and status flags
          atomic_store(&(segment->status), DEFAULT);
          atomic_store(&(segment->owner), RM_TX);
        }
        else
        {
          // Freeing allocated space
          free(segment->data);
          segment->data = NULL;
        }
      }
      else
      {
        // Commiting writes
        memcpy(segment->data, (char *)(segment->data) + segment->size, segment->size);

        // Reseting all the locks
        bzero((char *)(segment->data) + (segment->size << 1), (segment->size / region->align) * sizeof(tx_t));
      }

      // Resetting owner and status flags
      atomic_store(&(segment->owner), NO_TX);
      atomic_store(&(segment->status), DEFAULT);
    }

    // Resetting n_write_slots
    region->batcher.n_write_slots = WRITE_TX_PER_EPOCH_MAX;

    // Moving to next epoch
    atomic_fetch_add(&(region->batcher.counter), 1);
  }
  else if (tx != RO_TX)
  {
    UnlockBatcher(&(region->batcher));
    WaitForNextBatcherEpoch(&(region->batcher));
    return true;
  }

  UnlockBatcher(&(region->batcher));

  return true;
}

static inline Segment *LookupSegment(const Region *region, const void *source)
{
  // For each segment in region
  for (size_t i = region->index - 1; i < region->index; --i)
  {
    // Segment has been deleted
    if (atomic_load(&(region->segments[i].owner)) == RM_TX)
    {
      return NULL;
    }

    // Check if source is contained within segment's range
    if ((char *)source >= (char *)region->segments[i].data && (char *)source < (char *)region->segments[i].data + region->segments[i].size)
    {
      return region->segments + i;
    }
  }
  return NULL;
}

bool Lock(Region *region, Segment *segment, tx_t tx, void *target, size_t size)
{
  // Beggining of the control words
  size_t base_index = ((char *)target - (char *)segment->data) / region->align;

  // Getting the beggining of the controls words
  atomic_tx *controls = (atomic_tx *)((char *)segment->data + (segment->size << 1)) + base_index;

  // For each requested word
  size_t max = size / region->align;
  for (size_t i = 0; i < max; ++i)
  {
    tx_t expected1 = 0, expected2 = -tx;
    if (!(atomic_compare_exchange_strong(controls + i, &expected1, tx) || expected1 == tx || atomic_compare_exchange_strong(controls + i, &expected2, tx)))
    {
      if (i > 1)
      {
        bzero(controls, (i - 1) * sizeof(tx_t));
      }

      // Someone else has already locked the word
      return false;
    }
  }

  // Lock was successful
  return true;
}

static inline void Undo(Region *region, tx_t tx)
{
  // For each segment in region
  for (size_t i = region->index - 1; i < region->index; --i)
  {
    Segment *segment = region->segments + i;

    // Undo malloc of new segment
    if ((atomic_load(&(segment->status)) == ADDED || atomic_load(&(segment->status)) == ADDED_AFTER_REMOVE) && tx == atomic_load(&segment->owner))
    {
      atomic_store(&(segment->owner), RM_TX);
    }
    else if (segment->data != NULL && atomic_load(&(segment->owner)) != RM_TX)
    {
      // Reset segment in case its ours
      if (atomic_load(&(segment->owner)) == tx)
      {
        atomic_store(&(segment->owner), NO_TX);
        atomic_store(&(segment->status), DEFAULT);
      }

      // Control words
      atomic_tx *controls = (atomic_tx *)((char *)segment->data + (segment->size << 1));

      // For each word in the segment
      size_t max = segment->size / region->align;
      for (size_t j = 0; j < max; ++j)
      {
        // If we are the owner
        if (atomic_load(controls + j) == tx)
        {
          memcpy((char *)segment->data + segment->size + j * region->align, (char *)segment->data + j * region->align, region->align);
          atomic_store(controls + j, NO_TX);
        }
        else
        {
          // In case we have previously read
          tx_t expected = -tx;
          atomic_compare_exchange_weak(controls + j, &expected, NO_TX);
        }
      }
    }
  }

  // Leaving transaction
  Leave(region, tx);
}