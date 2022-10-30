// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
#include <xmmintrin.h>
#else

#include <sched.h>

#endif

// Internal headers
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

/*
Spinning locks instead of mutex locks?
The problem with mutexes is that putting threads to sleep and waking them up again are both rather expensive
operations, they'll need quite a lot of CPU instructions and thus also take some time.
On a multi-core/multi-CPU systems, with plenty of locks that are held for a very short amount
of time only, the time wasted for constantly putting threads to sleep and waking them up again
might decrease runtime performance noticeably. When using spinlocks instead, threads get the
chance to take advantage of their full runtime quantum (always only blocking for a very short time
period, but then immediately continue their work), leading to much higher processing throughput.
Here about 10 cores are being used!
https://stackoverflow.com/questions/5869825/when-should-one-use-a-spinlock-instead-of-mutex
*/
// Pause for few cycles.

static inline void pause()
{
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
    _mm_pause();
#else
    sched_yield();
#endif
}

// -------------------------------------------------------------------------- //

// state variables
#define DEFAULT 0
#define REMOVED 1
#define ADDED 2
#define ADDED_REMOVED 3

#define BATCHER_NB_TX 10
#define MULTIPLE_READERS UINTPTR_MAX - BATCHER_NB_TX

static const tx_t read_only_tx = UINTPTR_MAX - 1;
static const tx_t destroy_tx = UINTPTR_MAX - 2;

struct map_elem
{
    size_t size;
    void *ptr;
    _Atomic(tx_t) my_status;
    _Atomic(int) status;
};

struct batcher
{
    atomic_ulong counter;
    atomic_ulong num_entered_proc;
    atomic_ulong num_writing_proc;
    atomic_ulong epoch;
    atomic_ulong acquire_lock;
    atomic_ulong permission;
};

// basic struct which handles everything. It contains an instance of the batcher,
// useful to handle the entrance of the threads in the critical zones
struct region
{
    size_t align;
    size_t align_real;
    atomic_ulong index;
    struct batcher batcher;
    struct map_elem *map_elem; // array of map_elem
};

// initialize a new batcher setting all the values to 0
void get_new_batcher(struct batcher *batcher)
{
    batcher->counter = BATCHER_NB_TX;
    batcher->num_entered_proc = 0;
    batcher->num_writing_proc = 0;
    batcher->epoch = 0;
    batcher->acquire_lock = 0; // take the lock
    batcher->permission = 0;
}

shared_t tm_create(size_t size, size_t align)
{
    struct region *region = (struct region *)malloc(sizeof(struct region));
    size_t align_real = align < sizeof(void *) ? sizeof(void *) : align;
    size_t control_size = size / align_real * sizeof(tx_t);

    region->index = 1;
    region->map_elem = malloc(getpagesize());
    memset(region->map_elem, 0, getpagesize());
    region->map_elem->size = size;
    // set initial default status
    region->map_elem->status = DEFAULT;
    region->map_elem->my_status = 0;
    // align and reserve space for control and two copies
    posix_memalign(&(region->map_elem->ptr), align_real, 2 * size + control_size);
    get_new_batcher(&(region->batcher));
    memset(region->map_elem->ptr, 0, 2 * size + control_size);
    region->align = align;
    region->align_real = align_real;

    return region;
}

// free memory of region: first remove all the memory pointed by map_elem, then free the memory occupied by region
void tm_destroy(shared_t shared)
{
    struct region *region = (struct region *)shared;
    for (size_t i = 0; i < region->index; i++)
    {
        free(region->map_elem[i].ptr);
    }
    free(region->map_elem);
    free(region);
}

// This function returns the portion of memory that should be considered while reading/writing
struct map_elem *get_segment(struct region *region, const void *source)
{
    // I am looking for the map_elem which points to an area of memory which correspond to a piece of memory pointed by source*.
    for (size_t i = 0; i < region->index; i++)
    {
        char *start = (char *)region->map_elem[i].ptr;
        if ((char *)source >= start && (char *)source < start + region->map_elem[i].size)
        {
            // look for the right pointer: i.e. for the one pointing at the same block of memory also pointed by start
            // if source points to a memory area between the start of the memory area pointed by ptr (owned by a specific map_elem[i]) and its end:
            return region->map_elem + i;
        }
    }
    return NULL;
}

size_t tm_size(shared_t shared)
{
    return ((struct region *)shared)->map_elem->size;
}

size_t tm_align(shared_t shared)
{
    return ((struct region *)shared)->align_real;
}

void *tm_start(shared_t shared)
{
    return ((struct region *)shared)->map_elem->ptr;
}

tx_t enter(struct batcher *batcher, bool is_ro)
{
    if (is_ro)
    {                                                                                                           // threads just read
        unsigned long attempt = atomic_fetch_add_explicit(&(batcher->acquire_lock), 1ul, memory_order_relaxed); /*On a multi-core/multi-CPU systems, with plenty of locks that are held for a very short amount of time only, the time wasted for constantly putting threads to sleep and waking them up again might decrease runtime performance noticeably. When using spinlocks instead, threads get the  chance to take advantage of their full runtime quantum*/
        // keep iterating until the value of the obtained attempt corresponds to the pass
        while (batcher->permission != attempt)
            pause();
        atomic_fetch_add_explicit(&(batcher->num_entered_proc), 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&(batcher->permission), 1, memory_order_relaxed);
        return read_only_tx;
    }
    else
    { // one or more processes write
        while (true)
        {
            unsigned long attempt = atomic_fetch_add_explicit(&(batcher->acquire_lock), 1, memory_order_relaxed);
            // spinning locks again
            while (batcher->permission != attempt)
                pause();

            if (batcher->counter == 0)
            {
                unsigned long int epoch = batcher->epoch;
                atomic_fetch_add_explicit(&(batcher->permission), 1, memory_order_relaxed);
                // spinning locks again
                while (batcher->epoch == epoch)
                    pause();
            }
            // If I can enter, then I enter and then update the state values according to a new entrance
            else
            {
                atomic_fetch_add_explicit(&(batcher->counter), -1, memory_order_relaxed);
                break;
            }
        }
        atomic_fetch_add_explicit(&(batcher->num_entered_proc), 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&(batcher->permission), 1, memory_order_relaxed);
        tx_t tx = atomic_fetch_add_explicit(&(batcher->num_writing_proc), 1, memory_order_relaxed) + 1;

        return tx; // return the number of the transaction
    }
}

tx_t tm_begin(shared_t shared, bool is_ro)
{
    return enter(&(((struct region *)shared)->batcher), is_ro);
}

void commit(struct region *region)
{
    for (size_t i = region->index - 1; i < region->index; i--)
    {
        struct map_elem *map_elem = region->map_elem + i;

        // decide here whether the specified block is to be committed or not
        if (map_elem->my_status == destroy_tx || (map_elem->my_status != 0 && (map_elem->status == REMOVED || map_elem->status == ADDED_REMOVED)))
        {
            // free this block
            unsigned long int previous = i + 1;
            if (atomic_compare_exchange_weak(&(region->index), &previous, i))
            {
                // free and reset original values
                free(map_elem->ptr);
                map_elem->status = DEFAULT;
                map_elem->my_status = 0;
                map_elem->ptr = NULL;
            }
            else
            {
                map_elem->my_status = destroy_tx;
                map_elem->status = DEFAULT;
            }
        }
        else
        {
            // reset original status values and commit
            map_elem->my_status = 0;
            map_elem->status = DEFAULT;

            // Commit changes
            memcpy(map_elem->ptr, ((char *)map_elem->ptr) + map_elem->size, map_elem->size);

            // Reset locks
            memset(((char *)map_elem->ptr) + 2 * map_elem->size, 0, map_elem->size / region->align * sizeof(tx_t));
        }
    }
}

alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target)
{
    struct region *region = (struct region *)shared;
    // increment index, create a pointer to the next available area of memmory and set the variables of that map_element
    unsigned long int index = atomic_fetch_add_explicit(&(region->index), 1, memory_order_relaxed);
    struct map_elem *map_elem = region->map_elem + index;
    map_elem->status = ADDED;
    map_elem->size = size;
    map_elem->my_status = tx; // set tx a the transaction to use
    size_t align_real = region->align_real;
    size_t control_size = size / align_real * sizeof(tx_t);
    void *ptr = NULL;
    if (unlikely(posix_memalign(&ptr, align_real, 2 * size + control_size) != 0))
        return nomem_alloc;
    // set space memory for control and 2 copies
    memset(ptr, 0, 2 * size + control_size);
    map_elem->ptr = ptr;
    *target = ptr;
    return success_alloc;
}

void leave(struct batcher *batcher, struct region *region, tx_t tx)
{
    unsigned long attempt = atomic_fetch_add_explicit(&(batcher->acquire_lock), 1, memory_order_relaxed);
    // keep iterating until the value of the obtained attempt corresponds to the pass
    while (batcher->permission != attempt)
        pause();
    // if I have at least one writing operarion inside the batcher
    if (atomic_fetch_add_explicit(&batcher->num_entered_proc, -1, memory_order_relaxed) == 1)
    {
        if (batcher->num_writing_proc)
        {
            commit(region); // commit the operation and add 1 epoch(one operation concluded)
            atomic_fetch_add_explicit(&(batcher->epoch), 1, memory_order_relaxed);
            // restore initial values
            batcher->num_writing_proc = 0;
            batcher->counter = BATCHER_NB_TX;
        }
        atomic_fetch_add_explicit(&(batcher->permission), 1, memory_order_relaxed);
    }
    // similar to what was int enter
    else if (tx != read_only_tx)
    {
        unsigned long int epoch = batcher->epoch;
        atomic_fetch_add_explicit(&(batcher->permission), 1, memory_order_relaxed);
        while (batcher->epoch == epoch)
            pause();
    }
    else
    {
        atomic_fetch_add_explicit(&(batcher->permission), 1, memory_order_relaxed);
    }
}

bool tm_end(shared_t shared, tx_t tx)
{
    leave(&((struct region *)shared)->batcher, (struct region *)shared, tx);
    return true;
}

void tm_rollback(struct region *region, tx_t tx)
{
    unsigned long int index = region->index;
    for (size_t i = 0; i < index; ++i)
    {
        struct map_elem *map_elem = region->map_elem + i;
        tx_t owner = map_elem->my_status;
        if (owner == tx && (map_elem->status == ADDED || map_elem->status == ADDED_REMOVED))
        {
            map_elem->my_status = destroy_tx;
        }
        else if (likely(owner != destroy_tx && map_elem->ptr != NULL))
        {
            if (owner == tx)
            {
                map_elem->status = DEFAULT;
                map_elem->my_status = 0;
            }
            size_t align = region->align;
            size_t size = map_elem->size;
            size_t nb = map_elem->size / region->align;
            char *ptr = map_elem->ptr;
            _Atomic(tx_t) volatile *controls = (_Atomic(tx_t) volatile *)(ptr + 2 * size);
            for (size_t j = 0; j < nb; j++)
            {
                if (controls[j] == tx)
                {
                    memcpy(ptr + j * align + size, ptr + j * align, align);
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
    leave(&(region->batcher), region, tx);
}

bool tm_read_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    struct region *region = (struct region *)shared;
    struct map_elem *map_elem = get_segment(region, source);
    size_t align = region->align_real;
    size_t index = ((char *)source - (char *)map_elem->ptr) / align;
    size_t num = size / align;
    _Atomic(tx_t) volatile *controls = ((_Atomic(tx_t) volatile *)(map_elem->ptr + map_elem->size * 2)) + index;
    for (size_t i = 0; i < num; ++i)
    {
        tx_t no_owner = 0;
        tx_t owner = atomic_load(controls + i);
        if (owner == tx)
        {
            memcpy(((char *)target) + i * align, ((char *)source) + i * align + map_elem->size, align);
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
            tm_rollback(region, tx);
            return false;
        }
    }
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
        return tm_read_write(shared, tx, source, size, target);
    }
}

bool lock_words(struct region *region, tx_t tx, struct map_elem *map_elem, void *target, size_t size)
{
    size_t index = ((char *)target - (char *)map_elem->ptr) / region->align;
    size_t nb = size / region->align;
    _Atomic(tx_t) volatile *controls = (_Atomic(tx_t) volatile *)((char *)map_elem->ptr + map_elem->size * 2) + index;
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

bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    struct region *region = (struct region *)shared;
    struct map_elem *map_elem = get_segment(region, target); // look for the specific map_elem according to the given target
    if (map_elem == NULL || !lock_words(region, tx, map_elem, target, size))
    {
        tm_rollback(region, tx);
        return false;
    }
    size_t offset = map_elem->size;
    memcpy((char *)target + offset, source, size);
    return true;
}

bool tm_free(shared_t shared, tx_t tx, void *segment)
{
    struct map_elem *map_elem = get_segment((struct region *)shared, segment);
    tx_t previous = 0;
    if (!(atomic_compare_exchange_strong(&map_elem->my_status, &previous, tx) || previous == tx))
    {
        tm_rollback((struct region *)shared, tx);
        return false; // need to roolback since the transaction was not committed
    }
    if (map_elem->status == ADDED)
    {
        map_elem->status = ADDED_REMOVED;
    }
    else
    {
        map_elem->status = REMOVED;
    }
    return true; // the transaction finished positively, can go on
}
