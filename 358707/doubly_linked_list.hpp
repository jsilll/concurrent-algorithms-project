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
    Node *head_{};

public:
    ~DoublyLinkedList()
    {
        this->Clear();
    }

    void Clear()
    {
        while (head_ != nullptr)
        {
            Node *head = head_->next;
            delete head_;
            head_ = head;
        }
    }

    void Push(Node *node)
    {
        node->next = head_;

        if (node->next != nullptr)
        {
            node->next->prev = node;
        }

        head_ = node;
    }

    Node *Pop()
    {
        Node *res = head_;
        if (head_ != nullptr)
        {
            head_ = head_->next;
        }
        return res;
    }
};