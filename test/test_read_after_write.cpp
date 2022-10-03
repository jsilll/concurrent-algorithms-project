#include <cassert>

#include "tm.hpp"

#define assertm(exp, msg) assert(((void)msg, exp))

int main([[maybe_unused]] int argc, [[maybe_unused]] char *argv[])
{
    // Creating a new Memory Region
    auto region = tm_create(4, 4);

    // Memory Region Getters
    assertm(tm_size(region) == 4, "tm_size() == 4");
    assertm(tm_align(region) == 4, "tm_align() == 4");
    auto start = tm_start(region);

    // Starting a transaction
    auto tx = tm_begin(region, false);

    int src = 1;
    tm_write(region, tx, &src, sizeof(src), start);

    int res;
    tm_read(region, tx, start, 4, &res);
    assertm(res == 1, "Read After Write is Successful");

    // tm_alloc
    // tm_free

    // Ending the transaction
    tm_end(region, tx);

    tm_destroy(region);
}