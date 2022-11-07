#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

#include <tm.h>

// External Headers
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// Internal Headers
#include "macros.h"
#include "short_pause.h"

// -------------------------------------------------------------------------- //

// TODO: figure this out, refactor and change names
#define BATCHER_NB_TX 12UL
#define MULTIPLE_READERS UINTPTR_MAX - BATCHER_NB_TX

typedef _Atomic(tx_t) atomic_tx_t;

// TODO: change names
typedef enum
{
    DESTROY_TX = UINTPTR_MAX - 2,
    READ_ONLY_TX = UINTPTR_MAX - 1,
} TranssactionStatus;

// TODO: change names
typedef enum
{
    ADDED_FLAG,
    DEFAULT_FLAG,
    REMOVED_FLAG,
    ADDED_REMOVED_FLAG,
} SegmentStatus;

typedef struct
{
    void *data;
    size_t size;            // Size of the segment
    atomic_int status;      // Whether this blocks need to be added or removed in case of rollback and commit
    atomic_tx_t status_owner; // Identifier of the lock owner
} Segment;

typedef struct
{
    atomic_ulong pass;
    atomic_ulong take;
    atomic_ulong epoch;
    atomic_ulong counter;
    atomic_ulong nb_entered;
    atomic_ulong nb_write_tx;
} Batcher;

typedef struct
{
    size_t align; // Claimed alignment of the shared memory region (in bytes)
    Batcher batcher;
    Segment *mapping;
    size_t align_alloc; // Actual alignment of the memory allocations (in bytes)
    atomic_ulong index;
} Region;

// -------------------------------------------------------------------------- //

void Commit(Region *region)
{
    atomic_thread_fence(memory_order_acquire);

    for (size_t i = region->index - 1ul; i < region->index; --i)
    {
        Segment *mapping = region->mapping + i;

        if (mapping->status_owner == DESTROY_TX ||
            (mapping->status_owner != 0 && (mapping->status == REMOVED_FLAG || mapping->status == ADDED_REMOVED_FLAG)))
        {
            // Free this block
            unsigned long int previous = i + 1;
            if (atomic_compare_exchange_weak(&(region->index), &previous, i))
            {
                free(mapping->data);
                mapping->data = NULL;
                mapping->status = DEFAULT_FLAG;
                mapping->status_owner = 0;
            }
            else
            {
                mapping->status_owner = DESTROY_TX;
                mapping->status = DEFAULT_FLAG;
            }
        }
        else
        {
            mapping->status_owner = 0;
            mapping->status = DEFAULT_FLAG;

            // Commit changes
            memcpy(mapping->data, ((char *)mapping->data) + mapping->size, mapping->size);

            // Reset locks
            memset(((char *)mapping->data) + 2 * mapping->size, 0, mapping->size / region->align * sizeof(tx_t));
        }
    };
    atomic_thread_fence(memory_order_release);
}

void Leave(Batcher *batcher, Region *region, tx_t tx)
{
    // Acquire status lock
    unsigned long ticket = atomic_fetch_add_explicit(&(batcher->take), 1ul, memory_order_relaxed);

    while (atomic_load_explicit(&(batcher->pass), memory_order_relaxed) != ticket)
        short_pause();

    atomic_thread_fence(memory_order_acquire);

    if (atomic_fetch_add_explicit(&batcher->nb_entered, -1ul, memory_order_relaxed) == 1ul)
    {
        if (atomic_load_explicit(&(batcher->nb_write_tx), memory_order_relaxed) > 0)
        {
            Commit(region);
            atomic_store_explicit(&(batcher->nb_write_tx), 0, memory_order_relaxed);
            atomic_store_explicit(&(batcher->counter), BATCHER_NB_TX, memory_order_relaxed);
            atomic_fetch_add_explicit(&(batcher->epoch), 1ul, memory_order_relaxed);
        }

        atomic_fetch_add_explicit(&(batcher->pass), 1ul, memory_order_release);
    }
    else if (tx != READ_ONLY_TX)
    {
        unsigned long int epoch = atomic_load_explicit(&(batcher->epoch), memory_order_relaxed);
        atomic_fetch_add_explicit(&(batcher->pass), 1ul, memory_order_release);

        while (atomic_load_explicit(&(batcher->epoch), memory_order_relaxed) == epoch)
            short_pause();
    }
    else
    {
        atomic_fetch_add_explicit(&(batcher->pass), 1ul, memory_order_release);
    }
}

Segment *GetSegment(Region *region, const void *source)
{
    for (size_t i = 0; i < region->index; ++i)
    {
        if (unlikely(region->mapping[i].status_owner == DESTROY_TX))
        {
            return NULL;
        }
        char *start = (char *)region->mapping[i].data;
        if ((char *)source >= start && (char *)source < start + region->mapping[i].size)
        {
            return region->mapping + i;
        }
    }

    return NULL;
}

void Rollback(Region *region, tx_t tx)
{
    unsigned long int index = region->index;
    for (size_t i = 0; i < index; ++i)
    {
        Segment *mapping = region->mapping + i;

        tx_t owner = mapping->status_owner;
        if (owner == tx && (mapping->status == ADDED_FLAG || mapping->status == ADDED_REMOVED_FLAG))
        {
            mapping->status_owner = DESTROY_TX;
        }
        else if (likely(owner != DESTROY_TX && mapping->data != NULL))
        {
            if (owner == tx)
            {
                mapping->status = DEFAULT_FLAG;
                mapping->status_owner = 0;
            }

            size_t align = region->align;
            size_t size = mapping->size;
            size_t nb = mapping->size / region->align;
            char *data = mapping->data;
            atomic_tx_t volatile *controls = (atomic_tx_t volatile *)(data + 2 * size);

            for (size_t j = 0; j < nb; ++j)
            {
                if (controls[j] == tx)
                {
                    memcpy(data + j * align + size, data + j * align, align);
                    controls[j] = 0;
                }
                else
                {
                    tx_t previous = 0ul - tx;
                    atomic_compare_exchange_weak(controls + j, &previous, 0);
                }
                atomic_thread_fence(memory_order_release);
            }
        }
    };

    Leave(&(region->batcher), region, tx);
}

bool LockWords(Region *region, tx_t tx, Segment *mapping, void *target, size_t size)
{
    size_t index = ((char *)target - (char *)mapping->data) / region->align;
    size_t nb = size / region->align;

    atomic_tx_t volatile *controls = (atomic_tx_t volatile *)((char *)mapping->data + mapping->size * 2) + index;

    for (size_t i = 0; i < nb; ++i)
    {
        tx_t previous = 0;
        tx_t previously_read = 0ul - tx;
        if (!(atomic_compare_exchange_strong_explicit(controls + i, &previous, tx, memory_order_acquire,
                                                      memory_order_relaxed) ||
              previous == tx ||
              atomic_compare_exchange_strong(controls + i, &previously_read, tx)))
        {

            if (i > 1)
            {
                memset((void *)controls, 0, (i - 1) * sizeof(tx_t));
                atomic_thread_fence(memory_order_release);
            }
            return false;
        }
    }
    return true;
}

// For some reason this is only used in tm_read??
bool tm_read_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    Region *region = (Region *)shared;
    Segment *mapping = GetSegment(region, source);
    if (unlikely(mapping == NULL))
    {
        Rollback(region, tx);
        return false;
    }

    size_t align = region->align_alloc;
    size_t index = ((char *)source - (char *)mapping->data) / align;
    size_t nb = size / align;

    atomic_tx_t volatile *controls = ((atomic_tx_t volatile *)(mapping->data + mapping->size * 2)) + index;

    atomic_thread_fence(memory_order_acquire);
    // Read the data
    for (size_t i = 0; i < nb; ++i)
    {
        tx_t no_owner = 0;
        tx_t owner = atomic_load(controls + i);
        if (owner == tx)
        {
            memcpy(((char *)target) + i * align, ((char *)source) + i * align + mapping->size, align);
        }
        else if (atomic_compare_exchange_strong(controls + i, &no_owner, 0ul - tx) ||
                 no_owner == 0ul - tx || no_owner == MULTIPLE_READERS ||
                 (no_owner > MULTIPLE_READERS &&
                  atomic_compare_exchange_strong(controls + i, &no_owner, MULTIPLE_READERS)))
        {
            memcpy(((char *)target) + i * align, ((char *)source) + i * align, align);
        }
        else
        {
            Rollback(region, tx);
            return false;
        }
    }
    return true;
}

// -------------------------------------------------------------------------- //

shared_t tm_create(size_t size, size_t align)
{
    size_t align_alloc = align < sizeof(void *) ? sizeof(void *) : align;

    // Init Region
    Region *region = malloc(sizeof(Region));
    if (unlikely(region == NULL))
    {
        return invalid_shared;
    }

    region->index = 1;
    region->align = align;
    region->align_alloc = align_alloc;

    // Init Region->Batcher
    region->batcher.pass = 0;
    region->batcher.take = 0;
    region->batcher.epoch = 0;
    region->batcher.nb_entered = 0;
    region->batcher.nb_write_tx = 0;
    region->batcher.counter = BATCHER_NB_TX;

    // Init Region->Mapping
    region->mapping = malloc(getpagesize());
    if (unlikely(region->mapping == NULL))
    {
        free(region);
        return invalid_shared;
    }
    memset(region->mapping, 0, getpagesize());

    region->mapping->size = size;
    region->mapping->status_owner = 0;
    region->mapping->status = DEFAULT_FLAG;

    // Init Region->Mapping->Ptr
    size_t control_size = (size / align_alloc) * sizeof(tx_t);
    if (unlikely(posix_memalign(&(region->mapping->data), align_alloc, (size << 1) + control_size) != 0))
    {
        free(region->mapping);
        free(region);
        return invalid_shared;
    }
    memset(region->mapping->data, 0, (size << 1) + control_size);

    return region;
}

void tm_destroy(shared_t shared)
{
    Region *region = shared;
    for (size_t i = 0; i < region->index; ++i)
        free(region->mapping[i].data);
    free(region->mapping);
    free(region);
}

void *tm_start(shared_t shared)
{
    return ((Region *)shared)->mapping->data;
}

size_t tm_size(shared_t shared)
{
    return ((Region *)shared)->mapping->size;
}

size_t tm_align(shared_t shared)
{
    return ((Region *)shared)->align_alloc;
}

tx_t tm_begin(shared_t shared, bool is_ro)
{
    Region *region = shared;

    if (is_ro)
    {
        unsigned long ticket = atomic_fetch_add_explicit(&(region->batcher.take), 1ul, memory_order_relaxed);

        while (atomic_load_explicit(&(region->batcher.pass), memory_order_relaxed) != ticket)
            short_pause();

        atomic_thread_fence(memory_order_acquire);

        atomic_fetch_add_explicit(&(region->batcher.nb_entered), 1ul, memory_order_relaxed);

        atomic_fetch_add_explicit(&(region->batcher.pass), 1ul, memory_order_release);

        return READ_ONLY_TX;
    }
    else
    {
        while (true)
        {
            unsigned long ticket = atomic_fetch_add_explicit(&(region->batcher.take), 1ul, memory_order_relaxed);
            while (atomic_load_explicit(&(region->batcher.pass), memory_order_relaxed) != ticket)
                short_pause();
            atomic_thread_fence(memory_order_acquire);

            if (atomic_load_explicit(&(region->batcher.counter), memory_order_relaxed) == 0)
            {
                unsigned long int epoch = atomic_load_explicit(&(region->batcher.epoch), memory_order_relaxed);
                atomic_fetch_add_explicit(&(region->batcher.pass), 1ul, memory_order_release);

                while (atomic_load_explicit(&(region->batcher.epoch), memory_order_relaxed) == epoch)
                    short_pause();
                atomic_thread_fence(memory_order_acquire);
            }
            else
            {
                atomic_fetch_add_explicit(&(region->batcher.counter), -1ul, memory_order_release);
                break;
            }
        }
        atomic_fetch_add_explicit(&(region->batcher.nb_entered), 1ul, memory_order_relaxed);
        atomic_fetch_add_explicit(&(region->batcher.pass), 1ul, memory_order_release);

        tx_t tx = atomic_fetch_add_explicit(&(region->batcher.nb_write_tx), 1ul, memory_order_relaxed) + 1ul;
        atomic_thread_fence(memory_order_release);

        return tx;
    }
}

bool tm_end(shared_t shared, tx_t tx)
{
    Leave(&((Region *)shared)->batcher, (Region *)shared, tx);
    return true;
}

bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    if (likely(tx == READ_ONLY_TX))
    {
        // Read the data
        memcpy(target, source, size);
        return true;
    }
    else
    {
        return tm_read_write(shared, tx, source, size, target);
    }
}

bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    Region *region = (Region *)shared;
    Segment *mapping = GetSegment(region, target);

    if (mapping == NULL || !LockWords(region, tx, mapping, target, size))
    {
        Rollback(region, tx);
        return false;
    }

    memcpy((char *)target + mapping->size, source, size);

    return true;
}

alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target)
{
    Region *region = (Region *)shared;
    unsigned long int index = atomic_fetch_add_explicit(&(region->index), 1ul, memory_order_relaxed);

    Segment *mapping = region->mapping + index;
    mapping->status_owner = tx;
    mapping->size = size;
    mapping->status = ADDED_FLAG;

    size_t align_alloc = region->align_alloc;
    size_t control_size = size / align_alloc * sizeof(tx_t);

    void *data = NULL;
    if (unlikely(posix_memalign(&data, align_alloc, 2 * size + control_size) != 0))
        return nomem_alloc;

    memset(data, 0, 2 * size + control_size);
    *target = data;
    mapping->data = data;

    return success_alloc;
}

bool tm_free(shared_t shared, tx_t tx, void *segment)
{
    Segment *mapping = GetSegment((Region *)shared, segment);

    tx_t previous = 0;
    if (mapping == NULL || !(atomic_compare_exchange_strong(&mapping->status_owner, &previous, tx) || previous == tx))
    {
        Rollback((Region *)shared, tx);
        return false;
    }

    if (mapping->status == ADDED_FLAG)
    {
        mapping->status = ADDED_REMOVED_FLAG;
    }
    else
    {
        mapping->status = REMOVED_FLAG;
    }

    return true;
}
