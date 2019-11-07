	# qemu -kernel starts at 0x1000. the instructions
        # there seem to be provided by qemu, as if it
        # were a ROM. the code at 0x1000 jumps to
        # 0x8000000, the _start function here,
        # in machine mode. each CPU starts here.
.section .data
.globl stack0
.section .text
.globl start
.section .text
.globl _entry
# 从kernel.ld连接脚本可以看到，整个系统的入口就是_entry
# 这个片段就是设置好每个hart的sp，然后跳转到start函数处
# 每个hart的栈空间大小为4KB，所有的栈已经定义在start.c中
_entry:
	# set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + (hartid * 4096)
        la sp, stack0
        li a0, 1024*4
	csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
	# jump to start() in start.c
        call start
junk:
        j junk
