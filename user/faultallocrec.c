/* Test user-level fault handler -- alloc pages to fix faults */

#include <inc/lib.h>

bool
handler(struct UTrapframe *utf) {
    void *addr = (void *)utf->utf_fault_va;

    cprintf("fault %lx\n", (unsigned long)addr);
    cprintf("value at fault = %d\n", *(int *)addr);

    return 1;
}

void
umain(int argc, char **argv) {
    add_pgfault_handler(handler);
    cprintf("%s\n", (char *)0xBeefDead);
}
