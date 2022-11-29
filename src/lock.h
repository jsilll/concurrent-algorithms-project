#pragma once

#include <pthread.h>
#include <stdbool.h>

/**
 * @brief A lock that can only be taken exclusively. Contrarily to shared locks,
 * exclusive locks have wait/wake_up capabilities.
 */
typedef struct _lock_t
{
    pthread_mutex_t mutex;
    pthread_cond_t cv;
} lock_t;

/** Initialize the given lock.
 * @param lock Lock to initialize
 * @return Whether the operation is a success
 **/
bool lock_init(lock_t *lock)
{
    return pthread_mutex_init(&(lock->mutex), NULL) == 0 && pthread_cond_init(&(lock->cv), NULL) == 0;
}

/** Clean up the given lock.
 * @param lock Lock to clean up
 **/
void lock_cleanup(lock_t *lock)
{
    pthread_mutex_destroy(&(lock->mutex));
    pthread_cond_destroy(&(lock->cv));
}

/** Wait and acquire the given lock.
 * @param lock Lock to acquire
 * @return Whether the operation is a success
 **/
bool lock_acquire(lock_t *lock)
{
    return pthread_mutex_lock(&(lock->mutex)) == 0;
}

/** Release the given lock.
 * @param lock Lock to release
 **/
void lock_release(lock_t *lock)
{
    pthread_mutex_unlock(&(lock->mutex));
}

/** Wait until woken up by a signal on the given lock.
 *  The lock is released until lock_wait completes at which point it is acquired
 *  again. Exclusive lock access is enforced.
 * @param lock Lock to release (until woken up) and wait on.
 **/
void lock_wait(lock_t *lock)
{
    pthread_cond_wait(&(lock->cv), &(lock->mutex));
}

/** Wake up all threads waiting on the given lock.
 * @param lock Lock on which other threads are waiting.
 **/
void lock_wake_up(lock_t *lock)
{
    pthread_cond_broadcast(&(lock->cv));
}
