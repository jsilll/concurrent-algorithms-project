#pragma once

#include <map>
#include <memory>
#include <atomic>
#include <unordered_set>

struct Transaction
{
    uint32_t rv;                                            // Read-Version
    uint32_t wv{0};                                         // Write-Version
    bool ro;                                                // Read Only
    std::unordered_set<std::uintptr_t> read_set;                  // Set Of Read Words
    std::map<std::uintptr_t, std::unique_ptr<char[]>> write_set; // Target Word Src Word Mapping
};