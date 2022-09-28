#pragma once

#include "doubly_linked_list.hpp"

/**
 * @brief Transaction
 *
 */
struct Transaction
{
    /**
     * @brief Maintains the information
     * associated with a write within a
     * transaction.
     *
     */
    struct WriteLog
    {
        Segment *segment;
        size_t size;
        void *value;
    };

    /**
     * @brief Maintains the information
     * associated with a read within a
     * transaction.
     *
     */
    struct ReadLog
    {
        Segment *segment;
    };

    bool commited = true;
    bool read_only = false;
    DoublyLinkedList<ReadLog> rset;
    DoublyLinkedList<WriteLog> wset;
};
