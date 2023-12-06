#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

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
  // 内核发生中断异常的回调函数kernelvec
  // 根据前面在start.c中的设置，发生异常后，陷入到supervisor模式
  // 然后跳转到kernelvec处执行
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
  // 检查STATUS寄存器的SPP位，判断是不是0，也就是判断previous privilege是不是用户态
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  // 此时用户进程进入和内核态，所以设置stvec位kernelvec，来处理内核态发生的异常、中断
  w_stvec((uint64)kernelvec);
  // 通过cpu拿到当前执行的进程，注意现在的页表还是用户进程的页表
  struct proc *p = myproc();
  
  // save user program counter.
  p->tf->epc = r_sepc();
  
  // 根据scause判断是不是syscall
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    // 因为syscall也是异常的一种；发生异常的话，epc会被设置为导致异常的发生的那一条指令，在syscall中也就是ecall
    // 但是syscall在内核态处理完之后，需要继续向下执行，不需要重试那条指令，所以在这儿epc+4，移动到ecall的下一条指令
    p->tf->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();
    // 处理syscall的函数入口，我们仍然以initcode第一个用户进程的执行过程接着去kernel/syscall.c的syscall函数分析
    syscall();
  } else if((which_dev = devintr()) != 0){
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

  // 最后从内核继续返回到user mode
  usertrapret();
}

//
// return to user space
//
// 所有创建出来的进程都将从这儿返回到用户态
void
usertrapret(void)
{

  // 首先找到创建出来的进程，也就是initcode
  // 这个是通过cpus[hartid]->proc找到的
  struct proc *p = myproc();

  // turn off interrupts, since we're switching
  // now from kerneltrap() to usertrap().
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  // 设置中断向量地址为trampoline中的uservec
  // 因为接下来机器要进入用户态了，用户态的trapvec和内核态的trapvec是不一样的
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  // 把当前内核的上下文保护起来
  // 内核页表的基地址
  p->tf->kernel_satp = r_satp();         // kernel page table
  // 进程内核栈
  p->tf->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  // 用户态进程陷入内核之后的处理函数，设置为usertrap
  p->tf->kernel_trap = (uint64)usertrap;
  p->tf->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  // 设置previous privilege为0，也就是要回到user mode了
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  // 注意SPIE的意思是supervisor previous privilege，在这儿设置加上SPIE
  // 也就是说，在supervisor模式下，之前的中断使能是开启的，而发生中断之后处于supervisor模式的情况只有user mode
  // user mode --> supervisor mode --> machine mode；发生中断之后，mode只能从左到右单向改变
  // 总之就是x |= SSTATUS_SPIE允许了用户态中断
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  // 设置supervisor模式下的epc为p->tf->epc，注意这个值是在userinit()中设置为0，也就是用户进程的0地址
  w_sepc(p->tf->epc);

  // tell trampoline.S the user page table to switch to.
  // 这个satp就是用户进程自己的页表基地址
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  // 这个fn函数就是traampoline.S中的uservec函数
  // 在前面的初始化中(kvminit)，已经在内核页表中把trampoline映射到了TRAMPOLINE这个位置
  // 所以请注意，此时也是已经启用分页了，只是用的是内核页表，内核地址空间布局参考risv book Figure 3.3。
  // 所以fn函数就是trampoline.s中的userret函数
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  // 调用这个函数，参数分别是TRAPFRAME和satp
  // 注意在userret中第一步就是设置satp，也就是把内核页表换成了进程自己的页表
  // 所以第一个参数是TRAPFRAME，这个地址就是每个进程的进程空间的TRAPFRAME的地址，因为映射到了同一个地址
  // 问题来了，进程地址空间中的TRAPFRAME的内容是在哪儿设置的？initcode第一个进程是在userinit中设置的
  // 其实也只有两个需要设置p->tf->epc, p->tf->sp, 而在创建进程页表的时候，把p->tf这一页映射到了TRAPFRAME这个地址
  // 在fn中，也就是userret中load的其实也就是epc和sp，在userret的最后调用sret，将epc->pc，sstatus寄存器的SPP恢复
  // initcode也就正式进入用户态执行了
  // 注意在fn函数内部，先设置好satp寄存器，表示使用用户进程的页表，所以TRAPFRAME这个地址是相对进程页表来说的
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

  // 检查是哪一个设备中断，然后处理，得到which_dev
  if((which_dev = devintr()) == 0){
    printf("scause %p (%s)\n", scause, scause_desc(scause));
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  // 根据devintr，which_dev==2的情况只有一种，就是定时器中断
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

// 定时器中断的handler
void
clockintr()
{
  // ticks自增，然后wakeup唤醒sleep在ticks这个chan的进程
  // 再看一下这个chan是啥意思，在sys_sleep中，其实这个chan就是ticks的地址
  // 这儿wakeup也是ticks的地址，wakeup把sleep在这个chan上的进程全部唤醒
  // 这是啥意思呢？既然这样怎么区分sleep(5)，sleep(10)呢？
  // sleep，wakeup的chan都是同一个地址，那岂不是每次定时器中断都会把所有sleep的唤醒
  // 这是一种简单的做法，再回去看sys_sleep
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
      // 定时器中断
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
