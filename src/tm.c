#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

#include <sched.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include <tm.h>

// -------------------------------------------------------------------------- //

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

// TODO: refactor defines

#define DEFAULT 0
#define REMOVED 1
#define ADDED 2
#define ADDED_REMOVED 3

#define BATCHER_NB_TX 10
#define MULTIPLE_READERS UINTPTR_MAX - BATCHER_NB_TX

static const tx_t destroy_tx = UINTPTR_MAX - 2;
static const tx_t read_only_tx = UINTPTR_MAX - 1;

typedef struct
{
    void *data;
    size_t size;
    atomic_int status;
    _Atomic(tx_t) my_status; // TODO: rename this variable
} Segment;

typedef struct
{
    atomic_ulong lock;
    atomic_ulong epoch;
    atomic_ulong counter;
    atomic_ulong permission;
    atomic_ulong num_entered_proc; // TODO: rename
    atomic_ulong num_writing_proc; // TODO: rename
} Batcher;

typedef struct
{
    size_t align;
    Batcher batcher;
    Segment *segment;
    size_t align_real; // TODO: rename
    atomic_ulong index;
} Region;

// -------------------------------------------------------------------------- //

/**
 * @brief Gets the Segment Ptr
 * 
 * @param region 
 * @param source 
 * @return Segment* 
 */
static inline Segment *GetSegment(Region *region, const void *source)
{
    // This function returns the
    // portion of memory that should
    // be considered while reading/writing

    // I am looking for the segment which points to an area of memory which correspond to a piece of memory pointed by source*.
    for (size_t i = 0; i < region->index; i++)
    {
        char *start = (char *)region->segment[i].data;
        if ((char *)source >= start && (char *)source < start + region->segment[i].size)
        {
            // look for the right pointer: i.e. for the one pointing at the same block of memory also pointed by start
            // if source points to a memory area between the start of the memory area pointed by data (owned by a specific segment[i]) and its end:
            return region->segment + i;
        }
    }
    return NULL;
}

/**
 * @brief Leaves the Transaction
 * 
 * @param region 
 * @param tx 
 */
static inline void Leave(Region *region, tx_t tx)
{
    unsigned long attempt = atomic_fetch_add_explicit(&(region->batcher.lock), 1, memory_order_relaxed);
    // keep iterating until the value of the obtained attempt corresponds to the pass
    while (region->batcher.permission != attempt)
    {
        sched_yield();
    }
    // if I have at least one writing operarion inside the batcher
    if (atomic_fetch_add_explicit(&region->batcher.num_entered_proc, -1, memory_order_relaxed) == 1)
    {
        if (region->batcher.num_writing_proc)
        {
            // commit the operation and add 1 epoch(one operation concluded)
            for (size_t i = region->index - 1; i < region->index; i--)
            {
                Segment *segment = region->segment + i;

                // decide here whether the specified block is to be committed or not
                if (segment->my_status == destroy_tx || (segment->my_status != 0 && (segment->status == REMOVED || segment->status == ADDED_REMOVED)))
                {
                    // free this block
                    unsigned long int previous = i + 1;
                    if (atomic_compare_exchange_weak(&(region->index), &previous, i))
                    {
                        // free and reset original values
                        free(segment->data);
                        segment->status = DEFAULT;
                        segment->my_status = 0;
                        segment->data = NULL;
                    }
                    else
                    {
                        segment->my_status = destroy_tx;
                        segment->status = DEFAULT;
                    }
                }
                else
                {
                    // reset original status values and commit
                    segment->my_status = 0;
                    segment->status = DEFAULT;

                    // Commit changes
                    memcpy(segment->data, ((char *)segment->data) + segment->size, segment->size);

                    // Reset locks
                    memset(((char *)segment->data) + 2 * segment->size, 0, segment->size / region->align * sizeof(tx_t));
                }
            }
            atomic_fetch_add_explicit(&(region->batcher.epoch), 1, memory_order_relaxed);
            // restore initial values
            region->batcher.num_writing_proc = 0;
            region->batcher.counter = BATCHER_NB_TX;
        }
        atomic_fetch_add_explicit(&(region->batcher.permission), 1, memory_order_relaxed);
    }
    // similar to what was int enter
    else if (tx != read_only_tx)
    {
        unsigned long int epoch = region->batcher.epoch;
        atomic_fetch_add_explicit(&(region->batcher.permission), 1, memory_order_relaxed);
        while (region->batcher.epoch == epoch)
        {
            sched_yield();
        }
    }
    else
    {
        atomic_fetch_add_explicit(&(region->batcher.permission), 1, memory_order_relaxed);
    }
}

/**
 * @brief Rollbacks the Transaction
 * 
 * @param region 
 * @param tx 
 */
static inline void Rollback(Region *region, tx_t tx)
{
    unsigned long int index = region->index;
    for (size_t i = 0; i < index; ++i)
    {
        Segment *segment = region->segment + i;
        tx_t owner = segment->my_status;
        if (owner == tx && (segment->status == ADDED || segment->status == ADDED_REMOVED))
        {
            segment->my_status = destroy_tx;
        }
        else if (likely(owner != destroy_tx && segment->data != NULL))
        {
            if (owner == tx)
            {
                segment->status = DEFAULT;
                segment->my_status = 0;
            }
            size_t align = region->align;
            size_t size = segment->size;
            size_t nb = segment->size / region->align;
            char *data = segment->data;
            _Atomic(tx_t) volatile *controls = (_Atomic(tx_t) volatile *)(data + 2 * size);
            for (size_t j = 0; j < nb; j++)
            {
                if (controls[j] == tx)
                {
                    memcpy(data + j * align + size, data + j * align, align);
                    controls[j] = 0;
                }
                else
                {
                    tx_t previous = 0 - tx;
                    atomic_compare_exchange_weak(controls + j, &previous, 0);
                }
            }
        }
    }
    Leave(region, tx);
}

/**
 * @brief Locks the words for a given transaction
 * 
 * @param region 
 * @param tx 
 * @param segment 
 * @param target 
 * @param size 
 * @return true 
 * @return false 
 */
static inline bool LockWords(Region *region, tx_t tx, Segment *segment, void *target, size_t size)
{
    size_t index = ((char *)target - (char *)segment->data) / region->align;
    size_t nb = size / region->align;
    _Atomic(tx_t) volatile *controls = (_Atomic(tx_t) volatile *)((char *)segment->data + segment->size * 2) + index;
    for (size_t i = 0; i < nb; i++)
    {
        tx_t previous = 0;
        tx_t previously_read = 0 - tx;
        if (!(atomic_compare_exchange_strong_explicit(controls + i, &previous, tx, memory_order_acquire, memory_order_relaxed) || previous == tx || atomic_compare_exchange_strong(controls + i, &previously_read, tx)))
        {
            if (i > 1)
            {
                memset((void *)controls, 0, (i - 1) * sizeof(tx_t));
            }
            return false;
        }
    }
    return true;
}

// -------------------------------------------------------------------------- //

shared_t tm_create(size_t size, size_t align)
{
    size_t align_real = align < sizeof(void *) ? sizeof(void *) : align;
    size_t control_size = size / align_real * sizeof(tx_t);

    Region *region = (Region *)malloc(sizeof(Region));

    // Initializing Region
    region->index = 1;
    region->align = align;
    region->align_real = align_real;

    // Initializing Batcher
    region->batcher.lock = 0; // take the lock
    region->batcher.epoch = 0;
    region->batcher.permission = 0;
    region->batcher.num_entered_proc = 0;
    region->batcher.num_writing_proc = 0;
    region->batcher.counter = BATCHER_NB_TX;

    // Initializing Segment
    region->segment = malloc(getpagesize());
    memset(region->segment, 0, getpagesize());
    region->segment->size = size;
    region->segment->my_status = 0;
    region->segment->status = DEFAULT;

    // Allocate aligned space for control and two copies in for segment->data
    posix_memalign(&(region->segment->data), align_real, 2 * size + control_size);
    memset(region->segment->data, 0, 2 * size + control_size);

    return region;
}

void tm_destroy(shared_t shared)
{
    // free memory of region: first remove all the
    // memory pointed by segment, then free the memory
    // occupied by region

    Region *region = (Region *)shared;
    for (size_t i = 0; i < region->index; i++)
    {
        free(region->segment[i].data);
    }
    free(region->segment);
    free(region);
}

size_t tm_size(shared_t shared)
{
    return ((Region *)shared)->segment->size;
}

size_t tm_align(shared_t shared)
{
    return ((Region *)shared)->align_real;
}

void *tm_start(shared_t shared)
{
    return ((Region *)shared)->segment->data;
}

tx_t tm_begin(shared_t shared, bool is_ro)
{
    Region *region = (Region *)shared;

    if (is_ro)
    {                                                                                                        // threads just read
        unsigned long attempt = atomic_fetch_add_explicit(&(region->batcher.lock), 1, memory_order_relaxed); /*On a multi-core/multi-CPU systems, with plenty of locks that are held for a very short amount of time only, the time wasted for constantly putting threads to sleep and waking them up again might decrease runtime performance noticeably. When using spinlocks instead, threads get the  chance to take advantage of their full runtime quantum*/
        // keep iterating until the value of the obtained attempt corresponds to the pass
        while (region->batcher.permission != attempt)
        {
            sched_yield();
        }
        atomic_fetch_add_explicit(&(region->batcher.num_entered_proc), 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&(region->batcher.permission), 1, memory_order_relaxed);
        return read_only_tx;
    }

    // one or more processes write
    while (true)
    {
        unsigned long attempt = atomic_fetch_add_explicit(&(region->batcher.lock), 1, memory_order_relaxed);
        // spinning locks again
        while (region->batcher.permission != attempt)
        {
            sched_yield();
        }

        if (region->batcher.counter == 0)
        {
            unsigned long int epoch = region->batcher.epoch;
            atomic_fetch_add_explicit(&(region->batcher.permission), 1, memory_order_relaxed);
            // spinning locks again
            while (region->batcher.epoch == epoch)
                sched_yield();
        }
        // If I can enter, then I enter and then update the state values according to a new entrance
        else
        {
            atomic_fetch_add_explicit(&(region->batcher.counter), -1, memory_order_relaxed);
            break;
        }
    }
    atomic_fetch_add_explicit(&(region->batcher.num_entered_proc), 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&(region->batcher.permission), 1, memory_order_relaxed);
    tx_t tx = atomic_fetch_add_explicit(&(region->batcher.num_writing_proc), 1, memory_order_relaxed) + 1;

    // return the number of the transaction
    return tx;
}

alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target)
{
    Region *region = (Region *)shared;
    // increment index, create a pointer to the next available area of memmory and set the variables of that map_element
    unsigned long int index = atomic_fetch_add_explicit(&(region->index), 1, memory_order_relaxed);
    Segment *segment = region->segment + index;
    segment->status = ADDED;
    segment->size = size;
    segment->my_status = tx; // set tx a the transaction to use
    size_t align_real = region->align_real;
    size_t control_size = size / align_real * sizeof(tx_t);
    void *data = NULL;
    if (unlikely(posix_memalign(&data, align_real, 2 * size + control_size) != 0))
    {
        return nomem_alloc;
    }
    // set space memory for control and 2 copies
    memset(data, 0, 2 * size + control_size);
    segment->data = data;
    *target = data;
    return success_alloc;
}

bool tm_end(shared_t shared, tx_t tx)
{
    Leave((Region *)shared, tx);
    return true;
}

bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    if (tx == read_only_tx)
    {                                 // read only, easy xcase
        memcpy(target, source, size); // now target contains what is pointed by the source
        return true;
    }
    else
    {
        Region *region = (Region *)shared;
        Segment *segment = GetSegment(region, source);
        size_t align = region->align_real;
        size_t index = ((char *)source - (char *)segment->data) / align;
        size_t num = size / align;
        _Atomic(tx_t) volatile *controls = ((_Atomic(tx_t) volatile *)(segment->data + segment->size * 2)) + index;
        for (size_t i = 0; i < num; ++i)
        {
            tx_t no_owner = 0;
            tx_t owner = atomic_load(controls + i);
            if (owner == tx)
            {
                memcpy(((char *)target) + i * align, ((char *)source) + i * align + segment->size, align);
            }
            else if (atomic_compare_exchange_strong(controls + i, &no_owner, 0 - tx) ||
                     no_owner == 0 - tx || no_owner == MULTIPLE_READERS ||
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
}

bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    Region *region = (Region *)shared;
    Segment *segment = GetSegment(region, target); // look for the specific segment according to the given target
    if (segment == NULL || !LockWords(region, tx, segment, target, size))
    {
        Rollback(region, tx);
        return false;
    }
    size_t offset = segment->size;
    memcpy((char *)target + offset, source, size);
    return true;
}

bool tm_free(shared_t shared, tx_t tx, void *seg)
{
    Segment *segment = GetSegment((Region *)shared, seg);
    tx_t previous = 0;
    if (!(atomic_compare_exchange_strong(&segment->my_status, &previous, tx) || previous == tx))
    {
        Rollback((Region *)shared, tx);
        return false; // need to roolback since the transaction was not committed
    }
    if (segment->status == ADDED)
    {
        segment->status = ADDED_REMOVED;
    }
    else
    {
        segment->status = REMOVED;
    }
    return true; // the transaction finished positively, can go on
}
