#pragma once

#include <atomic>

/**
 * @brief Inspired from
 * https://stackoverflow.com/questions/22594647/implement-a-c-lock-using-atomic-instructions
 *
 */
class SpinLock
{
private:
    uint32_t version{};
    std::atomic<bool> locked{};

public:
    void Lock();
    void Unlock();
};