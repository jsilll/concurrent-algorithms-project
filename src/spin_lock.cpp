/**
 * @file   spin_lock.cpp
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
 * @section DESCRIPTION
 *
 * Spin Lock Implementation
 **/

#include "spin_lock.hpp"

// External Headers
#include <thread>

bool SpinLock::Lock()
{
    int i{0};
    bool expected = false;
    while (i < 10)
    {
        if (locked_.compare_exchange_strong(expected, true))
        {
            return true;
        }

        i++;
        std::this_thread::sleep_for (std::chrono::milliseconds(100));
    }

    return false;
}

bool SpinLock::IsLocked()
{
    return locked_.load();
}

void SpinLock::Unlock()
{
    locked_.store(false);
}

void SpinLock::Unlock(unsigned int wv)
{
    version_.store(wv);
    locked_.store(false);
}

unsigned int SpinLock::Version() 
{
    return version_.load();
}