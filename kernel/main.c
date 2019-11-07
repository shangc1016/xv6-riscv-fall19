#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "defs.h"

volatile static int started = 0;

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  // 进入到这儿的时候，特权级是supervisor

  if(cpuid() == 0){
    // 模拟出来的console用的是uart串口设备,
    // 实际上是qemu模拟出uart设备，然后映射到地址空间的固定位置
    // 我们向这个固定地址读写就模拟调用了串口设备
    // 在init中，做了设备初始化，设置波特率等工作
    consoleinit();
    // printf打印就是调用uart串口设备，把字符输出给qemu
    // 同时需要处理占位符%d, %x等
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    // 初始化物理内存
    kinit();         // physical page allocator
    // 初始化内核页表
    kvminit();       // create kernel page table
    // 启用内核页表
    kvminithart();   // turn on paging
    // 整个系统维护一个进程数组，遍历这个数组，为每个进程初始化好内核栈
    procinit();      // process table
    // 初始化内核trap处理函数，中断向量
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    // 
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    // 初始化系统bcache的双向链表
    binit();         // buffer cache
    // 和上面的bcache类似，初始化inode缓存数组每一项的lock
    iinit();         // inode cache
    // 和上面类似，初始化打开文件数组的lock，每次access打开文件，都要先获取锁
    fileinit();      // file table
    virtio_disk_init(minor(ROOTDEV)); // emulated hard disk
    // 第一个用户进程
    userinit();      // first user process
    // 从userinit中返回之后，全局的proc数组已经有一个可以执行的进程了，此时还是在supervisor的内核态
    __sync_synchronize();
    started = 1;
    // 上面的初始化只需要初始化一次
  } else {
    // 上面初始化完了之后，再做一些hart本地的初始化
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    // 其他的核心只需要启用内核页表就行了，内核页表只有一份，是这些hart之间共享的
    kvminithart();    // turn on paging
    // 每个hart的trap handler也需要单独设置
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }
  // 最后所有的hart都会进入scheduler这个死循环，进行多任务调度
  scheduler();        
}
