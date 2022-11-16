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
    unsigned long int turn = atomic_fetch_add(&(region->batcher.take), 1);
    while (turn != atomic_load(&(region->batcher.turn)))
    {
      relinquish_cpu();
    }
    // Incrementing number of transactions that entered in batcher
    atomic_fetch_add(&(region->batcher.n_tx_entered), 1);
    // Giving away turn
    atomic_fetch_add(&(region->batcher.turn), 1);
    return RO_TX;
  }

  while (true)
  {
    // Waiting for our turn
    unsigned long int turn = atomic_fetch_add(&(region->batcher.take), 1);
    while (turn != atomic_load(&(region->batcher.turn)))
    {
      relinquish_cpu();
    }
    // We can proceed
    if (atomic_load(&(region->batcher.remaining_slots)) != 0)
    {
      atomic_fetch_add(&(region->batcher.remaining_slots), -1);
      break;
    }
    // Giving away turn
    atomic_fetch_add(&(region->batcher.turn), 1);
    // Waiting for next epoch
    unsigned long int last_epoch = atomic_load(&(region->batcher.epoch));
    while (last_epoch == atomic_load(&(region->batcher.epoch)))
    {
      relinquish_cpu();
    }
  }
  // Incrementing number of write transactions 
  tx_t tx = atomic_fetch_add(&(region->batcher.n_write_tx_entered), 1) + 1;
  // Incrementing number of transactions entered,
  atomic_fetch_add(&(region->batcher.n_tx_entered), 1);
  // Giving away turn
  atomic_fetch_add(&(region->batcher.turn), 1);
  return tx;
}

static inline bool Leave(Region *region, tx_t tx)
{
  // Waiting for our turn
  unsigned long int turn = atomic_fetch_add(&(region->batcher.take), 1);
  while (turn != atomic_load(&(region->batcher.turn)))
  {
    relinquish_cpu();
  }
  // Check if this is the last write transaction
  if (atomic_fetch_add(&region->batcher.n_tx_entered, -1) == 1 && atomic_load(&(region->batcher.n_write_tx_entered)))
  {
    // Write transaction
    for (size_t i = region->index - 1; i < region->index; --i)
    {
      Segment *segment = region->segments + i;
      // If this segment is meant to be deleted
      if (segment->owner == RM_SCHED || segment->status == RM || segment->status == RM_N_ADDED)
      {
        unsigned long int expected_index = i + 1;
        if (!atomic_compare_exchange_strong(&(region->index), &expected_index, i))
        {
          // Resetting owner and status flags
          segment->status = DEFAULT;
          segment->owner = RM_SCHED;
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
      segment->owner = NONE;
      segment->status = DEFAULT;
    }
    // Resetting counter, n_write_tx_entered
    atomic_store(&(region->batcher.remaining_slots), N_TX);
    atomic_store(&(region->batcher.n_write_tx_entered), 0);
    // Moving to next epoch
    atomic_fetch_add(&(region->batcher.epoch), 1);
  }
  else if (tx != RO_TX)
  {
    // Giving away turn
    atomic_fetch_add(&(region->batcher.turn), 1);
    // Waiting for the next epoch
    unsigned long int epoch = atomic_load(&(region->batcher.epoch));
    while (epoch == atomic_load(&(region->batcher.epoch)))
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
    if (region->segments[i].owner == RM_SCHED)
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
  size_t base_index = ((char *)target - (char *)segment->data) / region->align;
  // Getting the beggining of the controls words
  atomic_tx *controls = (atomic_tx *)((char *)segment->data + (segment->size << 1)) + base_index;
  // For each requested word
  for (size_t i = 0; i < size / region->align; ++i)
  {
    tx_t previous = 0, previously_read = -tx;
    if (!(atomic_compare_exchange_strong(controls + i, &previous, tx) || previous == tx || atomic_compare_exchange_strong(controls + i, &previously_read, tx)))
    {
      if (i > 1)
      {
        bzero(controls, (i - 1) * sizeof(tx_t));
      }
      return false;
    }
  }
  return true;
}

static inline void Undo(Region *region, tx_t tx)
{
  // For each segment in region
  for (size_t i = region->index - 1; i < region->index; --i)
  {
    Segment *segment = region->segments + i;
    // Undo malloc of new segment
    if ((segment->status == ADDED || segment->status == RM_N_ADDED) && tx == segment->owner)
    {
      segment->owner = RM_SCHED;
    }
    else if (segment->data != NULL && segment->owner != RM_SCHED)
    {
      // Reset segment in case its ours
      if (segment->owner == tx)
      {
        segment->owner = NONE;
        segment->status = DEFAULT;
      }
      // Control words
      atomic_tx *controls = (atomic_tx *)((char *)segment->data + (segment->size << 1));
      // For each word in the segment
      for (size_t j = 0; j < segment->size / region->align; ++j)
      {
        // If we are the owner
        if (atomic_load(controls + j) == tx)
        {
          memcpy((char *)segment->data + segment->size + j * region->align, (char *)segment->data + j * region->align, region->align);
          atomic_store(controls + j, NONE);
        }
        else
        {
          tx_t previously_read = -tx;
          atomic_compare_exchange_weak(controls + j, &previously_read, NONE);
        }
      }
    }
  }
  // Leaving transaction
  Leave(region, tx);
}

#endif