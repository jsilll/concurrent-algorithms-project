/**
 * @file   spin_lock.hpp
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
 * Spin Lock Interface
 **/

#pragma once

// External Headers
#include <atomic>
#include <thread>

/**
 * @brief Implmentation inspired from
 * https://stackoverflow.com/questions/22594647/implement-a-c-lock-using-atomic-instructions
 *
 */
class SpinLock
{
private:
    std::atomic_bool locked_{0};
    std::atomic_uint version_{0};

public:
    bool Lock()
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
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return false;
    }

    void Unlock()
    {
        locked_.store(false);
    }
    void Unlock(unsigned int wv)
    {
        version_.store(wv);
        locked_.store(false);
    }

    bool IsLocked()
    {
        return locked_.load();
    }

    unsigned int Version()
    {
        return version_.load();
    }
};