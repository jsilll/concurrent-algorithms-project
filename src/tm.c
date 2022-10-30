#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
#include <xmmintrin.h>
#else
#include <sched.h>
#endif

#include <tm.h>

// -------------------------------------------------------------------------- //

/** Define a proposition as likely true.
 * @param prop Proposition
 **/
#undef likely
#ifdef __GNUC__
#define likely(prop) \
    __builtin_expect((prop) ? 1 : 0, 1)
#else
#define likely(prop) \
    (prop)
#endif

/** Define a proposition as likely false.
 * @param prop Proposition
 **/
#undef unlikely
#ifdef __GNUC__
#define unlikely(prop) \
    __builtin_expect((prop) ? 1 : 0, 0)
#else
#define unlikely(prop) \
    (prop)
#endif

/** Define one or several attributes.
 * @param type... Attribute names
 **/
#undef as
#ifdef __GNUC__
#define as(type...) \
    __attribute__((type))
#else
#define as(type...)
#warning This compiler has no support for GCC attributes
#endif

// -------------------------------------------------------------------------- //

#define ADDED_FLAG 2
#define DEFAULT_FLAG 0
#define REMOVED_FLAG 1
#define ADDED_REMOVED_FLAG 3

#define BATCHER_NB_TX 12UL
#define MULTIPLE_READERS UINTPTR_MAX - BATCHER_NB_TX

static const tx_t destroy_tx = UINTPTR_MAX - 2UL;
static const tx_t read_only_tx = UINTPTR_MAX - 1UL;

// -------------------------------------------------------------------------- //

typedef struct
{
    void *ptr;
    size_t size;               // Size of the segment
    _Atomic int status;        // Whether this blocks need to be added or removed in case of rollback and commit
    _Atomic tx_t status_owner; // Identifier of the lock owner
} MappingEntry;

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
    size_t align;       // Claimed alignment of the shared memory region (in bytes)
    size_t align_alloc; // Actual alignment of the memory allocations (in bytes)

    Batcher batcher;
    atomic_ulong index;
    MappingEntry *mapping;
} Region;

// -------------------------------------------------------------------------- //

/**
 * @brief Causes the calling thread to relinquish the CPU.
 * The thread is moved to the end of the queue for its static
 * priority and a new thread gets to run.
 */
static inline void short_pause()
{
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
    _mm_pause();
#else
    sched_yield();
#endif
}

// -------------------------------------------------------------------------- //

void BathCommit(Region *region)
{
    atomic_thread_fence(memory_order_acquire);

    for (size_t i = region->index - 1ul; i < region->index; --i)
    {
        MappingEntry *mapping = region->mapping + i;

        if (mapping->status_owner == destroy_tx ||
            (mapping->status_owner != 0 && (mapping->status == REMOVED_FLAG || mapping->status == ADDED_REMOVED_FLAG)))
        {
            // Free this block
            unsigned long int previous = i + 1;
            if (atomic_compare_exchange_weak(&(region->index), &previous, i))
            {
                free(mapping->ptr);
                mapping->ptr = NULL;
                mapping->status = DEFAULT_FLAG;
                mapping->status_owner = 0;
            }
            else
            {
                mapping->status_owner = destroy_tx;
                mapping->status = DEFAULT_FLAG;
            }
        }
        else
        {
            mapping->status_owner = 0;
            mapping->status = DEFAULT_FLAG;

            // Commit changes
            memcpy(mapping->ptr, ((char *)mapping->ptr) + mapping->size, mapping->size);

            // Reset locks
            memset(((char *)mapping->ptr) + 2 * mapping->size, 0, mapping->size / region->align * sizeof(tx_t));
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
            BathCommit(region);
            atomic_store_explicit(&(batcher->nb_write_tx), 0, memory_order_relaxed);
            atomic_store_explicit(&(batcher->counter), BATCHER_NB_TX, memory_order_relaxed);
            atomic_fetch_add_explicit(&(batcher->epoch), 1ul, memory_order_relaxed);
        }

        atomic_fetch_add_explicit(&(batcher->pass), 1ul, memory_order_release);
    }
    else if (tx != read_only_tx)
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

MappingEntry *GetSegment(Region *region, const void *source)
{
    for (size_t i = 0; i < region->index; ++i)
    {
        if (unlikely(region->mapping[i].status_owner == destroy_tx))
        {
            return NULL;
        }
        char *start = (char *)region->mapping[i].ptr;
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
        MappingEntry *mapping = region->mapping + i;

        tx_t owner = mapping->status_owner;
        if (owner == tx && (mapping->status == ADDED_FLAG || mapping->status == ADDED_REMOVED_FLAG))
        {
            mapping->status_owner = destroy_tx;
        }
        else if (likely(owner != destroy_tx && mapping->ptr != NULL))
        {
            if (owner == tx)
            {
                mapping->status = DEFAULT_FLAG;
                mapping->status_owner = 0;
            }

            size_t align = region->align;
            size_t size = mapping->size;
            size_t nb = mapping->size / region->align;
            char *ptr = mapping->ptr;
            _Atomic(tx_t) volatile *controls = (_Atomic(tx_t) volatile *)(ptr + 2 * size);

            for (size_t j = 0; j < nb; ++j)
            {
                if (controls[j] == tx)
                {
                    memcpy(ptr + j * align + size, ptr + j * align, align);
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

bool LockWords(Region *region, tx_t tx, MappingEntry *mapping, void *target, size_t size)
{
    size_t index = ((char *)target - (char *)mapping->ptr) / region->align;
    size_t nb = size / region->align;

    _Atomic(tx_t) volatile *controls = (_Atomic(tx_t) volatile *)((char *)mapping->ptr + mapping->size * 2) + index;

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
    MappingEntry *mapping = GetSegment(region, source);
    if (unlikely(mapping == NULL))
    {
        Rollback(region, tx);
        return false;
    }

    size_t align = region->align_alloc;
    size_t index = ((char *)source - (char *)mapping->ptr) / align;
    size_t nb = size / align;

    _Atomic(tx_t) volatile *controls = ((_Atomic(tx_t) volatile *)(mapping->ptr + mapping->size * 2)) + index;

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
    if (unlikely(posix_memalign(&(region->mapping->ptr), align_alloc, (size << 1) + control_size) != 0))
    {
        free(region->mapping);
        free(region);
        return invalid_shared;
    }
    memset(region->mapping->ptr, 0, (size << 1) + control_size);

    return region;
}

void tm_destroy(shared_t shared)
{
    Region *region = shared;
    for (size_t i = 0; i < region->index; ++i)
        free(region->mapping[i].ptr);
    free(region->mapping);
    free(region);
}

void *tm_start(shared_t shared)
{
    return ((Region *)shared)->mapping->ptr;
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

        return read_only_tx;
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
    if (likely(tx == read_only_tx))
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
    MappingEntry *mapping = GetSegment(region, target);

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

    MappingEntry *mapping = region->mapping + index;
    mapping->status_owner = tx;
    mapping->size = size;
    mapping->status = ADDED_FLAG;

    size_t align_alloc = region->align_alloc;
    size_t control_size = size / align_alloc * sizeof(tx_t);

    void *ptr = NULL;
    if (unlikely(posix_memalign(&ptr, align_alloc, 2 * size + control_size) != 0))
        return nomem_alloc;

    memset(ptr, 0, 2 * size + control_size);
    *target = ptr;
    mapping->ptr = ptr;

    return success_alloc;
}

bool tm_free(shared_t shared, tx_t tx, void *segment)
{
    MappingEntry *mapping = GetSegment((Region *)shared, segment);

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
