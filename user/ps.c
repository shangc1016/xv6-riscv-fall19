// 通过syscall直接进入内核，参考initcode
#include "user.h"

int main() {

    ps();
    exit(0);
}