/**
 * @file   tm.cpp
 * @author João Silveira <joao.freixialsilveira@epfl.ch>
 *
 * @section LICENSE
 *
 * Copyright © 2018-2021 Sébastien Rouault.
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
 **/

#pragma once

#ifdef __GNUC__
/**
 * @brief Define a proposition as likely true.
 *
 * @param prop
 * @return long
 */
inline long likely(long prop)
{
    return __builtin_expect(prop, true);
}

/**
 * @brief Define a proposition as likely false.
 *
 * @param prop
 * @return long
 */
inline long unlikely(long prop)
{
    return __builtin_expect(prop, false);
}
#else
inline long likely(long prop)
{
    return prop;
}

inline long unlikely(long prop)
{
    return prop;
}
#endif