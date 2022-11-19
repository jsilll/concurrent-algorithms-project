#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

#include "memory.h"
#include "basic_operations.h"

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 **/
shared_t tm_create(size_t size, size_t align)
{
  size_t true_align = align < sizeof(void *) ? sizeof(void *) : align;

  // Allocating Memory for the region
  Region *region = malloc(sizeof(Region));
  if (region == NULL)
  {
    return invalid_shared;
  }

  // Initializing Region
  region->align = align;
  region->true_align = true_align;
  atomic_store(&(region->index), 1);

  // Initializing region->batcher
  atomic_store(&(region->batcher.turn), 0);
  atomic_store(&(region->batcher.last_turn), 0);
  atomic_store(&(region->batcher.counter), 0);
  atomic_store(&(region->batcher.n_entered), 0);
  atomic_store(&(region->batcher.n_write_entered), 0);
  atomic_store(&(region->batcher.n_write_slots), MAX_WRITE_TX_PER_EPOCH);

  // Allocating space for region->segments
  region->segments = malloc(getpagesize());
  if (region->segments == NULL)
  {
    free(region);
    return invalid_shared;
  }

  // Initializing region->segment
  memset(region->segments, 0, getpagesize());

  region->segments->size = size;
  atomic_store(&(region->segments->status), DEFAULT);
  atomic_store(&(region->segments->owner), NO_OWNER);

  // Allocating Space for region->segment->data
  size_t control_size = (size / true_align) * sizeof(tx_t);
  if (posix_memalign(&(region->segments->data), true_align, (size << 1) + control_size) != 0)
  {
    free(region->segments);
    free(region);
    return invalid_shared;
  }

  // Initializing region->segment->data
  memset(region->segments->data, 0, (size << 1) + control_size);

  return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy(shared_t shared)
{
  Region *region = shared;

  // Deallocating all the segments in the region
  for (size_t i = region->index; i < region->index; --i)
  {
    free(region->segments[i].data);
  }
  free(region->segments);

  // Deallocating region itself
  free(region);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
 **/
void *tm_start(shared_t shared) { return ((Region *)shared)->segments->data; }

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
 **/
size_t tm_size(shared_t shared) { return ((Region *)shared)->segments->size; }

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
 **/
size_t tm_align(shared_t shared) { return ((Region *)shared)->true_align; }

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin(shared_t shared, bool is_ro) { return Enter((Region *)shared, is_ro); }

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
 **/
bool tm_end(shared_t shared, tx_t tx) { return Leave((Region *)shared, tx); }

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
 **/
bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
  // If it's a read only transaction we only need to copy the contents of the memory
  if (tx == RO_OWNER)
  {
    memcpy(target, source, size);
    return true;
  }

  // Looking up segment
  Region *region = (Region *)shared;
  Segment *segment = LookupSegment(region, source);
  if (segment == NULL)
  {
    Undo(region, tx);
    return false;
  }

  // Getting control words
  size_t base_index = ((char *)source - (char *)segment->data) / region->align;
  atomic_tx *controls = ((atomic_tx *)((char *)segment->data + (segment->size << 1))) + base_index;

  // Reading the content of the memory
  size_t max = size / region->align;
  for (size_t i = 0; i < max; ++i)
  {
    tx_t expected = NO_OWNER;
    if (tx == atomic_load(controls + i))
    {
      // We are the owner
      memcpy(((char *)target) + i * region->true_align, ((char *)source) + i * region->true_align + segment->size, region->true_align);
    }
    else if (atomic_compare_exchange_strong(controls + i, &expected, -tx) || expected == -tx || expected == RO_OWNER || (expected > RO_OWNER && atomic_compare_exchange_strong(controls + i, &expected, RO_OWNER)))
    {
      // We have previously read it or the word has not owner yet
      memcpy(((char *)target) + i * region->true_align, ((char *)source) + i * region->true_align, region->true_align);
    }
    else
    {
      // We were not able to read the word, undo
      Undo(region, tx);

      // Read as unsuccessful
      return false;
    }
  }
  return true;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
 **/
bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
  Region *region = (Region *)shared;

  // Looking up segment
  Segment *segment = LookupSegment(region, target);
  if (segment == NULL)
  {
    Undo(region, tx);
    return false;
  }

  // Trying to locking all the words
  if (!Lock(region, segment, tx, target, size))
  {
    Undo(region, tx);
    return false;
  }

  // Copying the contents to the destination
  memcpy((char *)target + segment->size, source, size);

  return true;
}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
 **/
alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target)
{
  Region *region = (Region *)shared;

  // Allocating new segment
  unsigned long int index = atomic_fetch_add(&(region->index), 1);
  Segment *segment = region->segments + index;

  // Initializing new segment
  segment->size = size;
  atomic_store(&(segment->owner), tx);
  atomic_store(&(segment->status), ADDED);

  // Allocating memory for the segment's data + control
  size_t control_size = segment->size / region->align * sizeof(tx_t);
  if (posix_memalign(&(segment->data), region->true_align, (size << 1) + control_size) != 0)
  {
    return nomem_alloc;
  }

  // Initializing data and control
  memset(segment->data, 0, (size << 1) + control_size);

  *target = segment->data;
  return success_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
 **/
bool tm_free(shared_t shared, tx_t tx, void *seg)
{
  // Looking up segment
  Segment *segment = LookupSegment((Region *)shared, seg);
  if (segment == NULL)
  {
    Undo((Region*)shared, tx);
    return false;
  }

  // Verifying segment has no current owner
  tx_t expected = NO_OWNER;
  if (!(atomic_compare_exchange_strong(&segment->owner, &expected, tx) || expected == tx))
  {
    Undo((Region *)shared, tx);
    return false;
  }

  // Signaling on segment status that the segment should be removed
  int previous_status = atomic_load(&(segment->status));
  atomic_store(&(segment->status), previous_status == ADDED ? ADDED_AFTER_REMOVE : REMOVED);

  return true;
}
