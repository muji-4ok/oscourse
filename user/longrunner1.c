#include <inc/lib.h>

void
umain(int argc, char **argv) {
    cprintf("[1] Running long start\n");

    uint64_t x = 1;

    for (int i = 0; i < 1000 * 1000 * 1000; ++i) {
        x = x * 337 + x; 
    }

    cprintf("[1] Running long end: %lu\n", x);
}
