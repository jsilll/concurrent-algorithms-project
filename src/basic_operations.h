#ifndef _BASIC_OPERATIONS_H_
#define _BASIC_OPERATIONS_H_

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "macros.h"
#include "memory.h"
#include "relinquish_cpu.h"

static inline tx_t Enter(Region *region, bool is_ro)
{
  if (is_ro)
  {
    // Waiting for our turn
    unsigned long int turn = atomic_fetch_add(&(region->batcher.last_turn), 1);
    while (turn != atomic_load(&(region->batcher.turn)))
    {
      relinquish_cpu();
    }

    // Incrementing number of transactions that entered in batcher
    atomic_fetch_add(&(region->batcher.n_entered), 1);

    // Giving away our turn
    atomic_fetch_add(&(region->batcher.turn), 1);

    return RO_OWNER;
  }

  while (true)
  {
    // Waiting for our turn
    unsigned long int turn = atomic_fetch_add(&(region->batcher.last_turn), 1);
    while (turn != atomic_load(&(region->batcher.turn)))
    {
      relinquish_cpu();
    }

    // We can proceed
    if (atomic_load(&(region->batcher.n_write_slots)) != 0)
    {
      atomic_fetch_add(&(region->batcher.n_write_slots), -1);
      break;
    }

    // Giving away turn
    atomic_fetch_add(&(region->batcher.turn), 1);

    // Waiting for next epoch
    unsigned long int last = atomic_load(&(region->batcher.counter));
    while (last == atomic_load(&(region->batcher.counter)))
    {
      relinquish_cpu();
    }
  }

  // Incrementing number of write transactions that entered
  tx_t tx = atomic_fetch_add(&(region->batcher.n_write_entered), 1) + 1;

  // Incrementing number of transactions entered,
  atomic_fetch_add(&(region->batcher.n_entered), 1);

  // Giving away our turn
  atomic_fetch_add(&(region->batcher.turn), 1);

  return tx;
}

static inline bool Leave(Region *region, tx_t tx)
{
  // Waiting for our turn
  unsigned long int turn = atomic_fetch_add(&(region->batcher.last_turn), 1);
  while (turn != atomic_load(&(region->batcher.turn)))
  {
    relinquish_cpu();
  }

  // Check if this is the last write transaction
  if (atomic_fetch_add(&region->batcher.n_entered, -1) == 1 && atomic_load(&(region->batcher.n_write_entered)))
  {
    // Write transaction
    for (size_t i = region->index - 1; i < region->index; --i)
    {
      Segment *segment = region->segments + i;
      // If this segment is meant to be deleted
      if (atomic_load(&(segment->owner)) == RM_OWNER || atomic_load(&(segment->status)) == REMOVED || atomic_load(&(segment->status)) == ADDED_AFTER_REMOVE)
      {
        unsigned long int expected = i + 1;
        if (!atomic_compare_exchange_strong(&(region->index), &expected, i))
        {
          // Resetting owner and status flags
          atomic_store(&(segment->status), DEFAULT);
          atomic_store(&(segment->owner), RM_OWNER);
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
      atomic_store(&(segment->owner), NO_OWNER);
      atomic_store(&(segment->status), DEFAULT);
    }

    // Resetting n_write_slots
    atomic_store(&(region->batcher.n_write_slots), MAX_WRITE_TX_PER_EPOCH);

    // Resetting n_write_entered
    atomic_store(&(region->batcher.n_write_entered), 0);

    // Moving to next epoch
    atomic_fetch_add(&(region->batcher.counter), 1);
  }
  else if (tx != RO_OWNER)
  {
    // Giving away turn
    atomic_fetch_add(&(region->batcher.turn), 1);

    // Waiting for the next epoch for atomic consistency
    unsigned long int epoch = atomic_load(&(region->batcher.counter));
    while (epoch == atomic_load(&(region->batcher.counter)))
    {
      relinquish_cpu();
    }

    return true;
  }

  // Giving away turn
  atomic_fetch_add(&(region->batcher.turn), 1);

  return true;
}

static inline Segment *LookupSegment(const Region *region, const void *source)
{
  // For each segment in region
  for (size_t i = region->index - 1; i < region->index; --i)
  {
    // Segment has been deleted
    if (atomic_load(&(region->segments[i].owner)) == RM_OWNER)
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
      atomic_store(&(segment->owner), RM_OWNER);
    }
    else if (segment->data != NULL && atomic_load(&(segment->owner)) != RM_OWNER)
    {
      // Reset segment in case its ours
      if (atomic_load(&(segment->owner)) == tx)
      {
        atomic_store(&(segment->owner), NO_OWNER);
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
          atomic_store(controls + j, NO_OWNER);
        }
        else
        {
          // In case we have previously read
          tx_t expected = -tx;
          atomic_compare_exchange_weak(controls + j, &expected, NO_OWNER);
        }
      }
    }
  }

  // Leaving transaction
  Leave(region, tx);
}

#endif