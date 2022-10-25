#pragma once

#include <map>
#include <memory>
#include <atomic>
#include <unordered_set>

struct Transaction
{
    uint64_t rv;                                            // Read-Version
    uint64_t wv{0};                                         // Write-Version
    bool ro;                                         // Read Only
    std::unordered_set<void *> read_set;                    // Set Of Read Words
    std::map<uintptr_t, std::unique_ptr<char[]>> write_set; // Target Word Src Word Mapping
};