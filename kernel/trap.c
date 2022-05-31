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
// 进到这个函数说明进程让出了控制权，转而到内核中了。有几种可能：1、syscall；2、其他错误
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
  } else if(r_scause() == 13 || r_scause() == 15) {
      // 如果scause寄存器的值是13或者15，表示是缺页错误；在这儿再真正分配一个物理页面，并且进行页面虚拟地址的映射
      // 最后让进程继续回去执行就行，
      // 这儿没有考虑分配时报错咋整。
      // 这么做好像有一个坑，这样做的话，分配的vm地址就有一个窟窿，这个窟窿就是之前sbrk(n)的n，
      // 因为在uvmalloc中直接把myproc()->sz当做真正的vm末尾。但是这部分并没有真正分配，
      // 所以即使调用growproc也还是中间有个洞。
    //   growproc(PGSIZE);
      // 根据hints，scause寄存器放的是发生缺页的vm，所以这个才是那个“洞”所对应的地址，在那个地址分配一个物理页面
      // 伪代码如下；
      // uint64 true_vm_end = PGROUNDDOWN(r_scause());
      // pg = kalloc();
      // mappages(pg, true_vm_end);
      // end.
    uint64 vm = r_stval();
    if(vm > p->sz){
        p->killed = 1;
        goto killed;
    }
      
    // 只要访问地址小于sp栈地址，就说明访问越界了，
    uint64 sp = p->tf->sp;
    if(vm < sp){
        p->killed = 1;
        goto killed;
    }

    if(uvmlazyalloc(p->pagetable, vm) == -1){
        p->killed = 1;
        goto killed;
    }
    //   uint64 vm = r_stval();
    //   // 判断引起缺页的地址是不是大于sz
    // //   printf("vm=%p, sz=%p", vm, p->sz);
    //   if(vm > p->sz){
    //       p->killed = 1;
    //       goto killed;
    //   }
    //   // 判断引起缺页的地址是不是进程栈和数据段之间的的保护页面？
    //   uint64 sp = p->tf->sp;
    //   sp = PGROUNDDOWN(sp);
    //   if(vm > sp-PGSIZE && vm < sp) {
    //       p->killed = 1;
    //       goto killed;
    //   }

    //   char *mem = kalloc();    // 物理页面
    //   if(mem == 0){   // oom
    //     p->killed = 1;
    //     goto killed;
    //   }
    //   memset(mem, 0, PGSIZE);  // 初始化页面
    //   // 计算虚拟地址空间的sz
    //   vm = PGROUNDDOWN(vm);
    // //   printf("page fault trap; stval = %p\n", r_stval());
    // //   printf("vm= %p\n", vm);
    
      
    //   // 忘了没加PTE_U标记，但是这个为啥不加PYE_V标记呢?
    //   // 这儿不加PTE_V标记的原因是，这个标记为在函数mappages中添加，不需要手动添加；
    //   mappages(p->pagetable, vm, PGSIZE, (uint64)mem, PTE_U | PTE_W | PTE_R | PTE_X);
      
    // //   printf("ret = %d\n", ret);

  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }
killed:
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
// must be 4-byte aligned to fit in stvec.
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
    printf("scause %p\n", scause);
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
    }

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

