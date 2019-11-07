# Initial process execs /init.
# This code runs in user space.

#include "syscall.h"

# exec(init, argv)
# 这个很明白，就是调用sys_exec系统调用，参数a0="/init\0", a1=0
.globl start
start:
        la a0, init
        la a1, argv
        li a7, SYS_exec
        ecall
        # 调用ecall之后出发软件中断(ps. 在usertrapret中设置允许user mode的中断)
        # 把当前指令(ecall)的地址写入epc，然后跳转到之前设置好的stvec地址执行
        # kernel/start.c中，委托中断，异常转到supervisor模式
        # 在usertrapret函数中设置了stvec为uservec(在trampoline.S中定义)
        # 因此此处的ecall触发异常，跳转到supervisor模式下的uservec处执行
        # 我们直接去uservec查看

# for(;;) exit();
exit:
        li a7, SYS_exit
        ecall
        jal exit

# char init[] = "/init\0";
init:
  .string "/init\0"

# char *argv[] = { init, 0 };
.p2align 2
argv:
  .long init
  .long 0
