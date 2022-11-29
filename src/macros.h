#ifndef _MACROS_H_
#define _MACROS_H_

/**
 * @brief Define a proposition as likely true.
 * @param prop Proposition
 */
#undef likely
#ifdef __GNUC__
#define likely(prop) __builtin_expect((prop) ? 1 : 0, 1)
#else
#define likely(prop) (prop)
#endif

/**
 * @brief Define a proposition as likely false.
 * @param prop Proposition
 */
#undef unlikely
#ifdef __GNUC__
#define unlikely(prop) __builtin_expect((prop) ? 1 : 0, 0)
#else
#define unlikely(prop) (prop)
#endif

#endif