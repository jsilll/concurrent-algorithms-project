#include <tm.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

// -------------------------------------------------------------------------- //

#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
#include <xmmintrin.h>
#else
#include <sched.h>
#endif

// -------------------------------------------------------------------------- //

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

#undef likely
#ifdef __GNUC__
#define likely(prop) \
    __builtin_expect((prop) ? 1 : 0, 1)
#else
#define likely(prop) \
    (prop)
#endif

#undef unlikely
#ifdef __GNUC__
#define unlikely(prop) \
    __builtin_expect((prop) ? 1 : 0, 0)
#else
#define unlikely(prop) \
    (prop)
#endif

#undef as
#ifdef __GNUC__
#define as(type...) \
    __attribute__((type))
#else
#define as(type...)
#warning This compiler has no support for GCC attributes
#endif

// -------------------------------------------------------------------------- //

static inline void nap()
{
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
    _mm_pause();
#else
    sched_yield();
#endif
}

// -------------------------------------------------------------------------- //

enum Status
{
    DEFAULT_FLAG,
    REMOVED_FLAG,
    ADDED_FLAG,
    ADDED_REMOVED_FLAG
};

#define BATCHER_NB_TX 12
#define MULTIPLE_READERS UINTPTR_MAX - BATCHER_NB_TX

static const tx_t READ_ONLY_TX = UINTPTR_MAX - 1;
static const tx_t REMOVE_SCHEDULED = UINTPTR_MAX - 2;

// -------------------------------------------------------------------------- //

struct Segment
{
    void *data;
    std::size_t size;               // Size of the segment
    std::atomic<int> status;        // Whether this blocks need to be added or removed in case of rollback and commit
    std::atomic<tx_t> status_owner; // Identifier of the lock owner
};

struct Batcher
{
    std::atomic_ulong pass; // Ticket that acquires the lock
    std::atomic_ulong take; // Ticket the next thread takes
    std::atomic_ulong epoch;
    std::atomic_ulong counter;
    std::atomic_ulong nb_entered;
    std::atomic_ulong nb_write_tx;
};

struct Region
{
    Batcher batcher;
    Segment *segment;
    std::size_t align;       // Claimed alignment of the shared memory region (in bytes)
    std::size_t align_alloc; // Actual alignment of the memory allocations (in bytes)
    std::atomic_ulong index;
};

// -------------------------------------------------------------------------- //

tx_t enter(Batcher *batcher, bool is_ro)
{
    if (is_ro)
    {
        // Acquire status lock
        unsigned long ticket = batcher->take.fetch_add(1, std::memory_order_relaxed);
        while (batcher->pass.load(std::memory_order_relaxed) != ticket)
            nap();
        std::atomic_thread_fence(std::memory_order_acquire);

        batcher->nb_entered.fetch_add(1, std::memory_order_relaxed);

        // Release status lock
        batcher->pass.fetch_add(1, std::memory_order_release);

        // printf("enter read\n");
        return READ_ONLY_TX;
    }
    else
    {
        while (true)
        {
            unsigned long ticket = batcher->take.fetch_add(1, std::memory_order_relaxed);
            while (batcher->pass.load(std::memory_order_relaxed) != ticket)
                nap();
            std::atomic_thread_fence(std::memory_order_acquire);

            // Acquire status lock
            if (batcher->counter.load(std::memory_order_relaxed) == 0)
            {
                unsigned long int epoch = batcher->epoch.load(std::memory_order_relaxed);
                batcher->pass.fetch_add(1, std::memory_order_release);

                while (batcher->epoch.load(std::memory_order_relaxed) == epoch)
                    nap();
                std::atomic_thread_fence(std::memory_order_acquire);
            }
            else
            {
                atomic_fetch_add_explicit(&(batcher->counter), -1ul, std::memory_order_release);
                break;
            }
        }
        batcher->nb_entered.fetch_add(1, std::memory_order_relaxed);
        batcher->pass.fetch_add(1, std::memory_order_release);

        tx_t tx = batcher->nb_write_tx.fetch_add(1, std::memory_order_relaxed) + 1ul;
        std::atomic_thread_fence(std::memory_order_release);

        return tx;
    }
}

void batch_commit(Region *region)
{
    std::atomic_thread_fence(std::memory_order_acquire);

    for (std::size_t i = region->index - 1ul; i < region->index; --i)
    {
        Segment *mapping = region->segment + i;

        if (mapping->status_owner == REMOVE_SCHEDULED ||
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
                mapping->status_owner = REMOVE_SCHEDULED;
                mapping->status = DEFAULT_FLAG;
            }
        }
        else
        {
            mapping->status_owner = 0;
            mapping->status = DEFAULT_FLAG;

            // Commit changes
            std::memcpy(mapping->data, ((char *)mapping->data) + mapping->size, mapping->size);

            // Reset locks
            std::memset(((char *)mapping->data) + 2 * mapping->size, 0, mapping->size / region->align * sizeof(tx_t));
        }
    };
    std::atomic_thread_fence(std::memory_order_release);
}

void leave(Batcher *batcher, Region *region, tx_t tx)
{
    // Acquire status lock
    unsigned long ticket = batcher->take.fetch_add(1, std::memory_order_relaxed);
    while (batcher->pass.load(std::memory_order_relaxed) != ticket)
        nap();
    std::atomic_thread_fence(std::memory_order_acquire);

    if (batcher->nb_entered.fetch_add(-1, std::memory_order_relaxed) == 1ul)
    {
        if (batcher->nb_write_tx.load(std::memory_order_relaxed) > 0)
        {
            batch_commit(region);
            batcher->nb_write_tx.store(0, std::memory_order_relaxed);
            batcher->counter.store(BATCHER_NB_TX, std::memory_order_relaxed);
            batcher->epoch.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
        }
        batcher->pass.fetch_add(1, std::memory_order_release);
    }
    else if (tx != READ_ONLY_TX)
    {
        unsigned long int epoch = atomic_load_explicit(&(batcher->epoch), std::memory_order_relaxed);
        batcher->pass.fetch_add(1, std::memory_order_release);

        while (batcher->epoch.load(std::memory_order_relaxed) == epoch)
            nap();
    }
    else
    {
        batcher->pass.fetch_add(1, std::memory_order_release);
    }
}

Segment *get_segment(Region *region, const void *source)
{
    for (std::size_t i = 0; i < region->index; ++i)
    {
        if (unlikely(region->segment[i].status_owner == REMOVE_SCHEDULED))
        {
            // printf("get_segment NULL\n");
            return nullptr;
        }
        char *start = (char *)region->segment[i].data;
        if ((char *)source >= start && (char *)source < start + region->segment[i].size)
        {
            return region->segment + i;
        }
    }

    return nullptr;
}

// -------------------------------------------------------------------------- //

shared_t tm_create(std::size_t size, std::size_t align)
{
    Region *region = (Region *)malloc(sizeof(Region));
    if (unlikely(!region))
    {
        return invalid_shared;
    }

    std::size_t align_alloc = align < sizeof(void *) ? sizeof(void *) : align;
    std::size_t control_size = size / align_alloc * sizeof(tx_t);

    region->index = 1;
    region->segment = (Segment *)malloc(getpagesize());
    if (unlikely(region->segment == NULL))
    {
        return invalid_shared;
    }
    memset(region->segment, 0, getpagesize());
    region->segment->size = size;
    region->segment->status = DEFAULT_FLAG;
    region->segment->status_owner = 0;

    if (unlikely(posix_memalign(&(region->segment->data), align_alloc, 2 * size + control_size) != 0))
    {
        free(region->segment);
        free(region);
        return invalid_shared;
    }

    region->batcher.counter = BATCHER_NB_TX;
    region->batcher.nb_entered = 0;
    region->batcher.nb_write_tx = 0;

    region->batcher.pass = 0;
    region->batcher.take = 0;
    region->batcher.epoch = 0;

    // Do we store a pointer to the control location
    memset(region->segment->data, 0, 2 * size + control_size);
    region->align = align;
    region->align_alloc = align_alloc;

    return region;
}

void tm_destroy(shared_t shared)
{
    Region *region = (Region *)shared;

    for (size_t i = 0; i < region->index; ++i)
    {
        free(region->segment[i].data);
    }

    free(region->segment);
    free(region);
}

void *tm_start(shared_t shared)
{
    return ((Region *)shared)->segment->data;
}

std::size_t tm_size(shared_t shared)
{
    return ((Region *)shared)->segment->size;
}

size_t tm_align(shared_t shared)
{
    return ((Region *)shared)->align_alloc;
}

tx_t tm_begin(shared_t shared, bool is_ro)
{
    return enter(&(((Region *)shared)->batcher), is_ro);
}

void tm_rollback(Region *region, tx_t tx)
{
    unsigned long int index = region->index;
    for (size_t i = 0; i < index; ++i)
    {
        Segment *mapping = region->segment + i;

        tx_t owner = mapping->status_owner;
        if (owner == tx && (mapping->status == ADDED_FLAG || mapping->status == ADDED_REMOVED_FLAG))
        {
            mapping->status_owner = REMOVE_SCHEDULED;
        }
        else if (likely(owner != REMOVE_SCHEDULED && mapping->data != NULL))
        {
            if (owner == tx)
            {
                mapping->status = DEFAULT_FLAG;
                mapping->status_owner = 0;
            }

            size_t align = region->align;
            size_t size = mapping->size;
            size_t nb = mapping->size / region->align;
            char *ptr = (char *)mapping->data;
            std::atomic<tx_t> volatile *controls = (std::atomic<tx_t> volatile *)(ptr + 2 * size);

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
                std::atomic_thread_fence(std::memory_order_release);
            }
        }
    };

    leave(&(region->batcher), region, tx);
}

bool tm_end(shared_t shared, tx_t tx)
{
    leave(&((Region *)shared)->batcher, (Region *)shared, tx);
    return true;
}

bool lock_words(Region *region, tx_t tx, Segment *mapping, void *target, std::size_t size)
{
    size_t index = ((char *)target - (char *)mapping->data) / region->align;
    size_t nb = size / region->align;

    std::atomic<tx_t> volatile *controls = (std::atomic<tx_t> volatile *)((char *)mapping->data + mapping->size * 2) + index;

    for (size_t i = 0; i < nb; ++i)
    {
        tx_t previous = 0;
        tx_t previously_read = 0ul - tx;
        if (!(atomic_compare_exchange_strong_explicit(controls + i, &previous, tx, std::memory_order_acquire, std::memory_order_relaxed) || previous == tx ||
              atomic_compare_exchange_strong(controls + i, &previously_read, tx)))
        {
            // printf("Unable to lock %lu - %lu --- %lu %lu %lu\n", tx, i+index, previous, previously_read, 0ul - tx);

            if (i > 1)
            {
                // printf("Rollback lock i: %ld \t index: %ld\n", i-1, index);
                memset((void *)controls, 0, (i - 1) * sizeof(tx_t));
                atomic_thread_fence(std::memory_order_release);
            }
            return false;
        }
    }
    return true;
}

bool tm_read_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    Region *region = (Region *)shared;
    Segment *mapping = get_segment(region, source);
    if (unlikely(mapping == NULL))
    {
        // printf("Rollback in read !!!");
        tm_rollback(region, tx);
        return false;
    }

    size_t align = region->align_alloc;
    size_t index = ((char *)source - (char *)mapping->data) / align;
    size_t nb = size / align;

    std::atomic<tx_t> volatile *controls = ((std::atomic<tx_t> volatile *)(mapping->data + mapping->size * 2)) + index;

    std::atomic_thread_fence(std::memory_order_acquire);
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
            tm_rollback(region, tx);
            return false;
        }
    }
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
        // printf("%lu read-write\n", tx);
        return tm_read_write(shared, tx, source, size, target);
    }
}

bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    // printf("%lu write\n", tx);
    Region *region = (Region *)shared;
    Segment *mapping = get_segment(region, target);

    if (mapping == NULL || !lock_words(region, tx, mapping, target, size))
    {
        tm_rollback(region, tx);
        return false;
    }

    memcpy((char *)target + mapping->size, source, size);
    return true;
}

alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target)
{
    Region *region = (Region *)shared;
    unsigned long int index = region->index.fetch_add(1, std::memory_order_relaxed);
    // printf("alloc ------------------ %lu\n", index);

    Segment *mapping = region->segment + index;
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
    mapping->data = ptr;

    return success_alloc;
}

bool tm_free(shared_t shared, tx_t tx, void *segment)
{
    Segment *mapping = get_segment((Region *)shared, segment);

    tx_t previous = 0;
    if (mapping == NULL || !(mapping->status_owner.compare_exchange_strong(previous, tx) || previous == tx))
    {
        tm_rollback((Region *)shared, tx);
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