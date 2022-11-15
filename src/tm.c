#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

#include "memory.h"

shared_t tm_create(size_t size, size_t align) {
  size_t align_alloc = align < sizeof(void *) ? sizeof(void *) : align;
  // Init Region
  Region *region = malloc(sizeof(Region));
  if (unlikely(region == NULL)) {
    return invalid_shared;
  }
  region->index = 1;
  region->align = align;
  region->align_alloc = align_alloc;
  // Init Region->Batcher
  region->batcher.turn = 0;
  region->batcher.take = 0;
  region->batcher.epoch = 0;
  region->batcher.n_tx_entered = 0;
  region->batcher.n_write_tx_entered = 0;
  region->batcher.counter = BATCHER_NB_TX;
  // Init Region->Segment
  region->segment = malloc(getpagesize());
  if (unlikely(region->segment == NULL)) {
    free(region);
    return invalid_shared;
  }
  memset(region->segment, 0, getpagesize());
  region->segment->size = size;
  region->segment->owner = NO_OWNER;
  region->segment->status = DEFAULT_FLAG;
  // Init Region->Segment->Ptr
  size_t control_size = (size / align_alloc) * sizeof(tx_t);
  if (unlikely(posix_memalign(&(region->segment->data), align_alloc, (size << 1) + control_size) != 0)) {
    free(region->segment);
    free(region);
    return invalid_shared;
  }
  memset(region->segment->data, 0, (size << 1) + control_size);
  return region;
}

void tm_destroy(shared_t shared) {
  Region *region = shared;
  for (size_t i = 0; i < region->index; ++i) {
    free(region->segment[i].data);
  }
  free(region->segment);
  free(region);
}

void *tm_start(shared_t shared) { return ((Region *)shared)->segment->data; }

size_t tm_size(shared_t shared) { return ((Region *)shared)->segment->size; }

size_t tm_align(shared_t shared) { return ((Region *)shared)->align_alloc; }

tx_t tm_begin(shared_t shared, bool is_ro) { return Enter((Region *)shared, is_ro); }

bool tm_end(shared_t shared, tx_t tx) { return Leave((Region *)shared, tx); }

bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target) {
  if (likely(tx == READ_ONLY_TX)) {
    memcpy(target, source, size);
    return true;
  } else {
    Region *region = (Region *)shared;
    Segment *segment = LookupSegment(region, source);
    if (unlikely(segment == NULL)) {
      Undo(region, tx);
      return false;
    }

    size_t align = region->align_alloc;
    size_t index = ((char *)source - (char *)segment->data) / align;
    size_t nb = size / align;

    atomic_tx volatile *controls = ((atomic_tx volatile *)((char*)segment->data + (segment->size << 1))) + index;

    // Read the data
    for (size_t i = 0; i < nb; ++i) {
      tx_t no_owner = NO_OWNER;
      tx_t owner = atomic_load(controls + i);
      if (owner == tx) {
        memcpy(((char *)target) + i * align,
               ((char *)source) + i * align + segment->size, align);
      } else if (atomic_compare_exchange_strong(controls + i, &no_owner,
                                                NO_OWNER - tx) ||
                 no_owner == NO_OWNER - tx || no_owner == MULTIPLE_READERS ||
                 (no_owner > MULTIPLE_READERS &&
                  atomic_compare_exchange_strong(controls + i, &no_owner,
                                                 MULTIPLE_READERS))) {
        memcpy(((char *)target) + i * align, ((char *)source) + i * align, align);
      } else {
        Undo(region, tx);
        return false;
      }
    }
    return true;
  }
}

bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target) {
  Region *region = (Region *)shared;
  Segment *segment = LookupSegment(region, target);
  if (segment == NULL || !Lock(region, segment, tx, target, size)) {
    Undo(region, tx);
    return false;
  }
  memcpy((char *)target + segment->size, source, size);
  return true;
}

alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target) {
  Region *region = (Region *)shared;
  unsigned long int index = atomic_fetch_add(&(region->index), 1);
  Segment *segment = region->segment + index;
  segment->owner = tx;
  segment->size = size;
  segment->status = ADDED;
  size_t align = region->align_alloc;
  size_t control_size = size / align * sizeof(tx_t);
  if (unlikely(posix_memalign(&(segment->data), align, (size << 1) + control_size) != 0)) {
    return nomem_alloc;
  }
  memset(segment->data, 0, (size << 1) + control_size);
  *target = segment->data;
  return success_alloc;
}

bool tm_free(shared_t shared, tx_t tx, void *seg) {
  tx_t expected = NO_OWNER;
  Segment *segment = LookupSegment((Region *)shared, seg);
  if (segment == NULL || !(atomic_compare_exchange_strong(&segment->owner, &expected, tx) || expected == tx)) {
    Undo((Region *)shared, tx);
    return false;
  }
  segment->status = segment->status == ADDED ? REMOVED_AFTER_ADD : REMOVED;
  return true;
}
