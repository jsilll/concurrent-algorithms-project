#pragma once

#include <stdatomic.h>

#include "yield.h"

/// @brief Represents a spin lock.
typedef struct _spin_lock
{
    atomic_flag flag;
} spin_lock_t;

/// @brief Initializes a spin lock.
static inline void spin_lock_init(spin_lock_t *lock)
{
    atomic_flag_clear(&(lock->flag));
}

/// @brief Locks a spin lock.
static inline void spin_lock_acquire(spin_lock_t *lock)
{
    while (atomic_flag_test_and_set_explicit(&(lock->flag), memory_order_acquire))
    {
        yield();
    }
}

/// @brief Unlocks a spin lock.
static inline void spin_lock_release(spin_lock_t *lock)
{
    atomic_flag_clear_explicit(&(lock->flag), memory_order_release);
}