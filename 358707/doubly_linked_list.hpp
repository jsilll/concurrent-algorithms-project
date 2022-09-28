#pragma once

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
        Node(Args &&...args) : content(std::forward<Args>(args)...)
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
        while (head_)
        {
            Node *tail = head_->next;
            delete head_;
            head_ = tail;
        }
    }

    void Push(Node *node)
    {
        node->next = head_;

        if (node->next)
        {
            node->next->prev = node;
        }

        head_ = node;
    }

    Node *Pop()
    {
        Node *res = head_;
        if (head_)
        {
            head_ = head_->next;
        }
        return res;
    }
};