#ifndef _RELINQUISH_CPU_H_
#define _RELINQUISH_CPU_H_

#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
#include <xmmintrin.h>
/**
 * @brief Causes the calling thread to relinquish the CPU.
 * The thread is moved to the end of the queue for its static
 * priority and a new thread gets to run.
 */
static inline void relinquish_cpu() { _mm_pause(); }
#else
#include <sched.h>
/**
 * @brief Causes the calling thread to relinquish the CPU.
 * The thread is moved to the end of the queue for its static
 * priority and a new thread gets to run.
 */
static inline void relinquish_cpu() { sched_yield(); }
#endif
#endif