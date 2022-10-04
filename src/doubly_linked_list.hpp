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
 * @section DESCRIPTION
 *
 * Doubly Linked List Interface
 **/

#pragma once

// External Headers
#ifdef DEBUG
#include <iostream>
#endif
#include <cstdlib>
#include <utility>

template <typename T>
class DoublyLinkedList
{
public:
    struct Node
    {
        T content;
        Node *next{};
        Node *prev{};

        template <class... Args>
        Node(Args &&...args)
            : content(std::forward<Args>(args)...)
        {
        }

        ~Node()
        {
            if (prev != nullptr)
            {
                prev->next = next;
            }

            if (next != nullptr)
            {
                next->prev = prev;
            }
        }
    };

private:
    Node *end_{};
    Node *begin_{};

public:
    ~DoublyLinkedList()
    {
        Clear();
    }

    Node *begin()
    {
        return begin_;
    }

    Node *end()
    {
        return end_;
    }

    void Clear()
    {
        while (begin_ != nullptr)
        {
#ifdef DEBUG
            std::cout << "[DEBUG] Deleting: " << begin_ << std::endl;
#endif
            Node *new_head = begin_->next;
            delete begin_;
            begin_ = new_head;
        }
    }

    void PushBack(Node *node)
    {
        node->prev = end_;

        if (node->prev != nullptr)
        {
            node->prev->next = node;
        }

        end_ = node;

        if (begin_ == nullptr)
        {
            begin_ = node;
        }
    }

    void PopFront()
    {
        if (begin_ != nullptr)
        {
            begin_ = begin_->next;
        }
    }

    void PopBack()
    {
        if (end_ != nullptr)
        {
            end_ = end_->prev;
        }
    }
};