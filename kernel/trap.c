#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fcntl.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

static const char *
scause_desc(uint64 stval);

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->tf->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->tf->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if (r_scause() == 0xd || r_scause() == 0xf) {

    // step1: 首先得到发生page fault的用户进程空间地址
    uint64 va = r_stval();

    // step2: 如果用户进程试图访问内核地址空间，直接kill进程 (对于usertests)
    if (KERNBASE <= va && va <= KERNBASE + 2000000) {
      p->killed = 1;
      usertrapret();
    }

    // step3: 如果发生page fault的地址处于进程栈指针中间的保护页面[sp, sp + PGSIZE]
    // 任何访问这块地址的情况都是错误的，直接杀死进程  (usertests)
    if (p->tf->sp - PGSIZE <= va && va <= p->tf->sp) {
      p->killed = 1;
      usertrapret();
    }

    // step4: 遍历进程的vma，查看引起当前page fault的地址访问属于哪个mmap
    int i;
    for (i = 0; i < MMAPSZ; i++) {
      if (p->vma[i].valid == 0) continue;
      if (p->vma[i].addr <= va && va < p->vma[i].addr + p->vma[i].length) {
        break;
      }
    }
    // step6: 这儿的kill进程有，一个是判断pageault是不是因为mmap的lazy分配引起的，
    // 如果不是的话，就说明是因为其他的原因，例如usertests中的sbrkfail测试点，
    // 扩张用户进程导致物理内存耗尽，而且在sbrk之后不判断返回值，而是直接使用这块内存，
    // 导致page fault。如果是这种情况，直接kill进程。
    if (i >= MMAPSZ) {
      p->killed = 1;
      usertrapret();
    }

    // step7: 执行到这儿，说明page fault是因为mmap的lazy分配导致的
    // 然后分配物理内存，准备mmap的lazy页面映射
    char *mem;
    if ((mem = kalloc()) == 0) {
      usertrapret();
    }
    memset(mem, 0, PGSIZE);

    // step8: 读vma[i].file文件的相应offset到kalloc分配的物理页面
    // step8.1: 计算得到本次page fault应该映射的文件页相对于文件起始地址的偏移
    // 因为在本lab中，mmap的文件偏移默认为0，所以直接vm - p->vma[i].addr就行
    uint64 offset = PGROUNDDOWN(va) - p->vma[i].addr;
    // step5.3: 使用readi读文件到mem页面，readfile定义在proc.c中
    if (readfilepage(p->vma[i].file, offset, mem) < 0) {
      usertrapret();
    }

    // step9: 根据mmap时候记录的prot设置页面映射的PTE访问属性
    int perm = 0;
    if (p->vma[i].prot & PROT_READ) perm |= PTE_R;
    if (p->vma[i].prot & PROT_WRITE) perm |= PTE_W;

    // 使用mappages映射页面，如果出错，就把kalloc的物理内存页面释放掉
    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm | PTE_U) != 0) {
      kfree(mem);
    }

  } else if ((which_dev = devintr()) != 0) {
    // ok
  } else {
    printf("usertrap(): unexpected scause %p (%s) pid=%d\n", r_scause(), scause_desc(r_scause()), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // turn off interrupts, since we're switching
  // now from kerneltrap() to usertrap().
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->tf->kernel_satp = r_satp();         // kernel page table
  p->tf->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->tf->kernel_trap = (uint64)usertrap;
  p->tf->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->tf->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p (%s)\n", scause, scause_desc(scause));
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ || irq == VIRTIO1_IRQ ){
      virtio_disk_intr(irq - VIRTIO0_IRQ);
    } else {
      // the PLIC sends each device interrupt to every core,
      // which generates a lot of interrupts with irq==0.
    }

    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

static const char *
scause_desc(uint64 stval)
{
  static const char *intr_desc[16] = {
    [0] "user software interrupt",
    [1] "supervisor software interrupt",
    [2] "<reserved for future standard use>",
    [3] "<reserved for future standard use>",
    [4] "user timer interrupt",
    [5] "supervisor timer interrupt",
    [6] "<reserved for future standard use>",
    [7] "<reserved for future standard use>",
    [8] "user external interrupt",
    [9] "supervisor external interrupt",
    [10] "<reserved for future standard use>",
    [11] "<reserved for future standard use>",
    [12] "<reserved for future standard use>",
    [13] "<reserved for future standard use>",
    [14] "<reserved for future standard use>",
    [15] "<reserved for future standard use>",
  };
  static const char *nointr_desc[16] = {
    [0] "instruction address misaligned",
    [1] "instruction access fault",
    [2] "illegal instruction",
    [3] "breakpoint",
    [4] "load address misaligned",
    [5] "load access fault",
    [6] "store/AMO address misaligned",
    [7] "store/AMO access fault",
    [8] "environment call from U-mode",
    [9] "environment call from S-mode",
    [10] "<reserved for future standard use>",
    [11] "<reserved for future standard use>",
    [12] "instruction page fault",
    [13] "load page fault",
    [14] "<reserved for future standard use>",
    [15] "store/AMO page fault",
  };
  uint64 interrupt = stval & 0x8000000000000000L;
  uint64 code = stval & ~0x8000000000000000L;
  if (interrupt) {
    if (code < NELEM(intr_desc)) {
      return intr_desc[code];
    } else {
      return "<reserved for platform use>";
    }
  } else {
    if (code < NELEM(nointr_desc)) {
      return nointr_desc[code];
    } else if (code <= 23) {
      return "<reserved for future standard use>";
    } else if (code <= 31) {
      return "<reserved for custom use>";
    } else if (code <= 47) {
      return "<reserved for future standard use>";
    } else if (code <= 63) {
      return "<reserved for custom use>";
    } else {
      return "<reserved for future standard use>";
    }
  }
}
