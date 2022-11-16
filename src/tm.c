#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

#include "memory.h"
#include "basic_operations.h"

shared_t tm_create(size_t size, size_t align)
{
  size_t true_align = align < sizeof(void *) ? sizeof(void *) : align;
  // Init Region
  Region *region = malloc(sizeof(Region));
  if (unlikely(region == NULL))
  {
    return invalid_shared;
  }
  region->index = 1;
  region->align = align;
  region->true_align = true_align;
  // Init Region->Batcher
  region->batcher.turn = 0;
  region->batcher.take = 0;
  region->batcher.epoch = 0;
  region->batcher.n_tx_entered = 0;
  region->batcher.n_write_tx_entered = 0;
  region->batcher.remaining_slots = N_TX;
  // Init Region->Segment
  region->segments = malloc(getpagesize());
  if (unlikely(region->segments == NULL))
  {
    free(region);
    return invalid_shared;
  }
  memset(region->segments, 0, getpagesize());
  region->segments->size = size;
  region->segments->owner = NONE;
  region->segments->status = DEFAULT;
  // Init Region->Segment->Ptr
  size_t control_size = (size / true_align) * sizeof(tx_t);
  if (unlikely(posix_memalign(&(region->segments->data), true_align, (size << 1) + control_size) != 0))
  {
    free(region->segments);
    free(region);
    return invalid_shared;
  }
  memset(region->segments->data, 0, (size << 1) + control_size);
  return region;
}

void tm_destroy(shared_t shared)
{
  Region *region = shared;
  for (size_t i = region->index; i < region->index; --i)
  {
    free(region->segments[i].data);
  }
  free(region->segments);
  free(region);
}

void *tm_start(shared_t shared) { return ((Region *)shared)->segments->data; }

size_t tm_size(shared_t shared) { return ((Region *)shared)->segments->size; }

size_t tm_align(shared_t shared) { return ((Region *)shared)->true_align; }

tx_t tm_begin(shared_t shared, bool is_ro) { return Enter((Region *)shared, is_ro); }

bool tm_end(shared_t shared, tx_t tx) { return Leave((Region *)shared, tx); }

bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
  if (likely(tx == RO_TX))
  {
    memcpy(target, source, size);
    return true;
  }
  else
  {
    Region *region = (Region *)shared;
    Segment *segment = LookupSegment(region, source);
    if (unlikely(segment == NULL))
    {
      Undo(region, tx);
      return false;
    }

    size_t align = region->true_align;
    size_t index = ((char *)source - (char *)segment->data) / align;
    size_t nb = size / align;

    atomic_tx volatile *controls = ((atomic_tx volatile *)((char *)segment->data + (segment->size << 1))) + index;

    // Read the data
    for (size_t i = 0; i < nb; ++i)
    {
      tx_t no_owner = NONE;
      tx_t owner = atomic_load(controls + i);
      if (owner == tx)
      {
        memcpy(((char *)target) + i * align, ((char *)source) + i * align + segment->size, align);
      }
      else if (atomic_compare_exchange_strong(controls + i, &no_owner, NONE - tx) ||
               no_owner == NONE - tx ||
               no_owner == M_RO_TX ||
               (no_owner > M_RO_TX && atomic_compare_exchange_strong(controls + i, &no_owner, M_RO_TX)))
      {
        memcpy(((char *)target) + i * align, ((char *)source) + i * align, align);
      }
      else
      {
        Undo(region, tx);
        return false;
      }
    }
    return true;
  }
}

bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
  Region *region = (Region *)shared;
  Segment *segment = LookupSegment(region, target);
  if (segment == NULL || !Lock(region, segment, tx, target, size))
  {
    Undo(region, tx);
    return false;
  }
  memcpy((char *)target + segment->size, source, size);
  return true;
}

alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target)
{
  Region *region = (Region *)shared;
  unsigned long int index = atomic_fetch_add(&(region->index), 1);
  Segment *segment = region->segments + index;
  segment->owner = tx;
  segment->size = size;
  segment->status = ADDED;
  size_t align = region->true_align;
  size_t control_size = size / align * sizeof(tx_t);
  if (unlikely(posix_memalign(&(segment->data), align, (size << 1) + control_size) != 0))
  {
    return nomem_alloc;
  }
  memset(segment->data, 0, (size << 1) + control_size);
  *target = segment->data;
  return success_alloc;
}

bool tm_free(shared_t shared, tx_t tx, void *seg)
{
  tx_t expected = NONE;
  Segment *segment = LookupSegment((Region *)shared, seg);
  if (segment == NULL || !(atomic_compare_exchange_strong(&segment->owner, &expected, tx) || expected == tx))
  {
    Undo((Region *)shared, tx);
    return false;
  }
  segment->status = segment->status == ADDED ? RM_N_ADDED : RM;
  return true;
}
