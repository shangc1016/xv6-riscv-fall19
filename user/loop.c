// #include "kernel/syscall.h"
#include "kernel/types.h"
#include "user/user.h"

int main() {
    uint64 x = 0;
    for(;;) {
        sleep(10);
        printf("running... x = %d\n", x++);
    }
}