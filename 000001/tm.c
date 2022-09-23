/**
 * @file   tm.c
 * @author Robin Mamie <robin.mamie@epfl.ch>
 *
 * @section LICENSE
 *
 * Copyright © 2018-2019 Robin Mamie.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version. Please see https://gnu.org/licenses/gpl.html
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * @section DESCRIPTION
 *
 * Lock-based transaction manager implementation used as the reference.
 **/

// Compile-time configuration
// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

// External headers
#include <pthread.h>
// #include <stdatomic.h>
// #include <stddef.h>
#include <stdlib.h>
#include <string.h>
// #if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
// #include <xmmintrin.h>
// #else
// #include <sched.h>
// #endif

// Internal headers
#include <tm.h>

#include "macros.h"

struct link
{
    struct link *prev; // Previous link in the chain
    struct link *next; // Next link in the chain
};

/** Link reset.
 * @param link Link to reset
 **/
static void link_reset(struct link *link)
{
    link->prev = link;
    link->next = link;
}

/** Link insertion before a "base" link.
 * @param link Link to insert
 * @param base Base link relative to which 'link' will be inserted
 **/
static void link_insert(struct link *link, struct link *base)
{
    struct link *prev = base->prev;
    link->prev = prev;
    link->next = base;
    base->prev = link;
    prev->next = link;
}

/** Link removal.
 * @param link Link to remove
 **/
static void link_remove(struct link *link)
{
    struct link *prev = link->prev;
    struct link *next = link->next;
    prev->next = next;
    next->prev = prev;
}

// -------------------------------------------------------------------------- //

struct lock_t
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

/** Initialize the given lock.
 * @param lock Lock to initialize
 * @return Whether the operation is a success
 **/
static bool lock_init(struct lock_t *lock)
{
    if (pthread_cond_init(&(lock->cond), NULL) != 0)
    {
        return false;
    }
    if (pthread_mutex_init(&(lock->mutex), NULL) != 0)
    {
        pthread_cond_destroy(&(lock->cond));
        return false;
    }
    return true;
}

/** Clean the given lock up.
 * @param lock Lock to clean up
 **/
static void lock_cleanup(struct lock_t *lock)
{
    pthread_mutex_destroy(&(lock->mutex));
    pthread_cond_destroy(&(lock->cond));
}

/** Wait and acquire the given lock.
 * @param lock Lock to acquire
 * @return Whether the operation is a success
 **/
static bool lock_acquire(struct lock_t *lock)
{
    return pthread_mutex_lock(&(lock->mutex)) == 0;
}

/** Release the given lock.
 * @param lock Lock to release
 **/
static void lock_release(struct lock_t *lock)
{
    pthread_mutex_unlock(&(lock->mutex));
}

static void lock_broadcast(struct lock_t *lock)
{
    pthread_cond_broadcast(&(lock->cond));
}

static void lock_wait(struct lock_t *lock)
{
    pthread_cond_wait(&(lock->cond), &(lock->mutex));
}

// -------------------------------------------------------------------------- //

static const tx_t read_only_tx = UINTPTR_MAX - 10;
static const tx_t read_write_tx = UINTPTR_MAX - 11;

struct region
{
    unsigned long nb_ro_transactions; // Number of currently executing read-only transactions
    unsigned long nb_rw_transactions; // Number of currently executing read-write transactions
    struct lock_t write_lock;         // Global write lock
    struct lock_t metadata_lock;      // Global write lock
    void *start;                      // Start of the shared memory region
    struct link allocs;               // Allocated shared memory regions
    size_t size;                      // Size of the shared memory region (in bytes)
    size_t align;                     // Claimed alignment of the shared memory region (in bytes)
    size_t align_alloc;               // Actual alignment of the memory allocations (in bytes)
    size_t delta_alloc;               // Space to add at the beginning of the segment for the link chain (in bytes)
};

shared_t tm_create(size_t size, size_t align)
{
    struct region *region = (struct region *)malloc(sizeof(struct region));
    if (unlikely(!region))
    {
        return invalid_shared;
    }
    size_t align_alloc = align < sizeof(void *) ? sizeof(void *) : align; // Also satisfy alignment requirement of 'struct link'
    if (unlikely(posix_memalign(&(region->start), align_alloc, size) != 0))
    {
        free(region);
        return invalid_shared;
    }
    if (unlikely(!lock_init(&(region->write_lock))))
    {
        free(region->start);
        free(region);
        return invalid_shared;
    }
    if (unlikely(!lock_init(&(region->metadata_lock))))
    {
        free(region->start);
        free(region);
        lock_cleanup(&(region->write_lock));
        return invalid_shared;
    }
    memset(region->start, 0, size);
    link_reset(&(region->allocs));
    region->nb_ro_transactions = 0;
    region->nb_rw_transactions = 0;
    region->size = size;
    region->align = align;
    region->align_alloc = align_alloc;
    region->delta_alloc = (sizeof(struct link) + align_alloc - 1) / align_alloc * align_alloc;
    return region;
}

void tm_destroy(shared_t shared)
{
    struct region *region = (struct region *)shared;
    struct link *allocs = &(region->allocs);
    while (true)
    { // Free allocated segments
        struct link *alloc = allocs->next;
        if (alloc == allocs)
            break;
        link_remove(alloc);
        free(alloc);
    }
    free(region->start);
    lock_cleanup(&(region->write_lock));
    lock_cleanup(&(region->metadata_lock));
    free(region);
}

void *tm_start(shared_t shared)
{
    return ((struct region *)shared)->start;
}

size_t tm_size(shared_t shared)
{
    return ((struct region *)shared)->size;
}

size_t tm_align(shared_t shared)
{
    return ((struct region *)shared)->align;
}

tx_t tm_begin(shared_t shared, bool is_ro)
{
    struct region *region = (struct region *)shared;
    lock_acquire(&(region->metadata_lock));
    if (is_ro)
    {
        while (region->nb_rw_transactions != 0UL)
        {
            lock_wait(&(region->metadata_lock));
        }
        region->nb_ro_transactions += 1UL;
        lock_release(&(region->metadata_lock));
        return read_only_tx;
    }
    else
    {
        while (region->nb_ro_transactions != 0UL)
        {
            lock_wait(&(region->metadata_lock));
        }
        region->nb_rw_transactions += 1UL;
        lock_release(&(region->metadata_lock));
        lock_acquire(&(region->write_lock));
        return read_write_tx;
    }
}

bool tm_end(shared_t shared, tx_t tx)
{
    struct region *region = (struct region *)shared;
    if (tx == read_only_tx)
    {
        lock_acquire(&(region->metadata_lock));
        lock_broadcast(&(region->metadata_lock));
        region->nb_ro_transactions -= 1UL;
        lock_release(&(region->metadata_lock));
    }
    else
    {
        lock_release(&(region->write_lock));
        lock_acquire(&(region->metadata_lock));
        lock_broadcast(&(region->metadata_lock));
        region->nb_rw_transactions -= 1UL;
        lock_release(&(region->metadata_lock));
    }
    return true;
}

bool tm_read(shared_t unused(shared), tx_t unused(tx), void const *source, size_t size, void *target)
{
    memcpy(target, source, size);
    return true;
}

bool tm_write(shared_t unused(shared), tx_t unused(tx), void const *source, size_t size, void *target)
{
    memcpy(target, source, size);
    return true;
}

alloc_t tm_alloc(shared_t shared, tx_t unused(tx), size_t size, void **target)
{
    size_t align_alloc = ((struct region *)shared)->align_alloc;
    size_t delta_alloc = ((struct region *)shared)->delta_alloc;
    void *segment;
    if (unlikely(posix_memalign(&segment, align_alloc, delta_alloc + size) != 0)) // Allocation failed
        return nomem_alloc;
    link_insert((struct link *)segment, &(((struct region *)shared)->allocs));
    segment = (void *)((uintptr_t)segment + delta_alloc);
    memset(segment, 0, size);
    *target = segment;
    return success_alloc;
}

bool tm_free(shared_t shared, tx_t unused(tx), void *segment)
{
    size_t delta_alloc = ((struct region *)shared)->delta_alloc;
    segment = (void *)((uintptr_t)segment - delta_alloc);
    link_remove((struct link *)segment);
    free(segment);
    return true;
}