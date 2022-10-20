/**
 * @file   memory.hpp
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
 * STM's memory layout Interface.
 **/

#pragma once

// External Headers
#include <new>
#include <tm.hpp>
#include <atomic>
#include <cerrno>
#include <vector>
#include <cstdlib>
#include <cstring>

// Internal Headers
#include "spin_lock.hpp"
#include "transaction.hpp"
#include "versioned_lock.hpp"
#include "segment_allocator.hpp"

struct SharedRegion
{
private:
    std::size_t align_;      // Memory Alignment
    std::atomic_uint gv_{0}; // Global Version

    SegmentManager manager_;                                                     // Segment Manager
    std::mutex descriptor_mutex_;                                                // Descriptor Mutex
    std::atomic<TransactionDescriptor *> current_{new TransactionDescriptor{0}}; // Transaction Descriptor

public:
    SharedRegion(size_t size, size_t align) noexcept
        : align_(align), manager_(size, align)
    {
    }

    ~SharedRegion() noexcept = default;

    SharedRegion(SharedRegion &&) = delete;
    SharedRegion &operator=(SharedRegion &&) = delete;

    SharedRegion(const SharedRegion &) = delete;
    SharedRegion &operator=(const SharedRegion &) = delete;

public:
    inline std::size_t align() const noexcept
    {
        return align_;
    }

    inline std::size_t size() const noexcept
    {
        return manager_.first().size();
    }

    inline ObjectId start() const noexcept
    {
        return manager_.start();
    }

public:
    bool end_tx(Transaction &tx) noexcept
    {
        if (tx.is_ro)
        {
            unref(tx.start_point);
            return true;
        }

        // First, try acquiring all locks in the write set
        auto it = tx.write_set.begin();

        std::unordered_set<std::size_t> acquired_locks;
        while (it != tx.write_set.end())
        {
            if (!it->obj.lock.try_lock(tx.start_time))
            {
                unlock_all(tx.write_set.begin(), it);
                abort(tx);
                return false;
            }
            acquired_locks.insert(it->addr.to_opaque());
            it++;
        }

        // Validate read set
        for (auto &read : tx.read_set)
        {
            if (acquired_locks.find(read.addr.to_opaque()) != acquired_locks.end())
            {
                continue;
            }
            if (!read.obj.lock.validate(tx.start_time))
            {
                unlock_all(tx.write_set.begin(), it);
                abort(tx);
                return false;
            }
        }

        {
            std::unique_lock<std::mutex> lock(descriptor_mutex_);
            commit_changes(tx);
        }

        return true;
    }

    Transaction *begin_tx(bool is_ro) noexcept
    {
        TransactionDescriptor *start_point;

        {
            std::unique_lock<std::mutex> lock(descriptor_mutex_);
            start_point = current_.load(std::memory_order_acquire);
            start_point->refcount.fetch_add(1, std::memory_order_acq_rel);
        }

        return new Transaction(is_ro, start_point, start_point->commit_time);
    }

    bool read_word(Transaction &tx, ObjectId src, char *dst) noexcept
    {
        auto &obj = manager_.find(src);
        if (tx.is_ro)
        {
            read_word_readonly(tx, obj, dst);
            return true;
        }

        if (auto entry = tx.find_write_entry(src))
        {
            std::memcpy(dst, entry->written.get(), align_);
            return true;
        }

        auto latest = obj.latest.load(std::memory_order_acquire);
        if (!obj.lock.validate(tx.start_time))
        {
            abort(tx);
            return false;
        }
        tx.read_set.push_back({src, obj});
        latest->read(dst, align_);
        return true;
    }

    bool write_word(Transaction &tx, const char *src, ObjectId dst) noexcept
    {
        if (auto entry = tx.find_write_entry(dst))
        {
            std::memcpy(entry->written.get(), src, align_);
            return true;
        }

        auto &obj = manager_.find(dst);
        auto written = clone(src, align_);
        tx.write_set.push_back({dst, obj, std::move(written)});
        return true;
    }

    void free(Transaction &tx, ObjectId addr) noexcept
    {
        if (manager_.find_segment(addr).mark_for_deletion())
        {
            tx.free_set.push_back(addr);
        }
    }

    bool allocate(Transaction &tx, std::size_t size, ObjectId *dest) noexcept
    {
        auto success = manager_.allocate(size, dest);
        if (success)
        {
            tx.alloc_set.push_back(*dest);
        }
        return success;
    }

private:
    static void unlock_all(std::vector<Transaction::WriteEntry>::iterator begin,
                           std::vector<Transaction::WriteEntry>::iterator end)
    {
        while (begin != end)
        {
            begin->obj.lock.unlock();
            begin++;
        }
    }

    static std::unique_ptr<char[]> clone(const char *word, std::size_t align)
    {
        auto copy = std::make_unique<char[]>(align);
        std::memcpy(copy.get(), word, align);
        return copy;
    }

private:
    void read_word_readonly(const Transaction &tx, const Object &obj, char *dst) const noexcept
    {
        auto ver = obj.latest.load(std::memory_order_acquire);
        while (ver->version > tx.start_time)
        {
            ver = ver->earlier;
        }
        ver->read(dst, align_);
    }

    void ref(TransactionDescriptor *desc) noexcept
    {
        if (desc == nullptr)
        {
            return;
        }
    }

    void unref(TransactionDescriptor *desc) noexcept
    {
        if (desc == nullptr)
        {
            return;
        }

        auto previous = desc->refcount.fetch_sub(1, std::memory_order_acq_rel);
        if (previous == 1)
        {
            unref(desc->next);
            commit_frees(*desc);
            delete desc;
        }
    }

    void abort(Transaction &tx) noexcept
    {
        for (auto segment : tx.alloc_set)
        {
            manager_.free(segment);
        }
        for (auto segment : tx.free_set)
        {
            manager_.find_segment(segment).cancel_deletion();
        }
        unref(tx.start_point);
    }

    void commit_changes(Transaction &tx) noexcept
    {
        auto cur_point = current_.load(std::memory_order_acquire);

        auto commit_time = cur_point->commit_time + 1;
        auto *descr = new TransactionDescriptor{commit_time};

        cur_point->next = descr;
        ref(descr);

        unref(cur_point);

        current_.store(descr, std::memory_order_release);

        descr->segments_to_delete = std::move(tx.free_set);

        for (auto &write : tx.write_set)
        {
            auto &obj = write.obj;

            auto *old_version = obj.latest.load(std::memory_order_acquire);

            auto *new_version = new ObjectVersion(std::move(write.written));

            new_version->version = commit_time;
            new_version->earlier = old_version;

            obj.latest.store(new_version, std::memory_order_release);
            descr->objects_to_delete.emplace_back(old_version);
            obj.lock.unlock(commit_time);
        }

        unref(tx.start_point);
    }

    void commit_frees(TransactionDescriptor &desc) noexcept
    {
        for (auto segm : desc.segments_to_delete)
        {
            manager_.free(segm);
        }
    }
};