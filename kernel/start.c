#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S needs one stack per CPU.
// 对于栈，要求设置地址按照16字节对齐，否则可能会出现'Instruction address misaligned'
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// scratch area for timer interrupt, one per CPU.
// 内核任务切换的时候，这个mscratch内容用来保护32个通用寄存器
uint64 mscratch0[NCPU * 32];

// assembly code in kernelvec.S for machine-mode timer interrupt.
extern void timervec();

// entry.S jumps here in machine mode on stack0.
void
start()
{
  // set M Previous Privilege mode to Supervisor, for mret.
  // 设置machine模式下的status寄存器，设置MPP位为supervisor，previous privilege
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  // set previous privilege to supervisor mode.
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  // 设置mepc寄存器为main，也就是接下来要跳转执行的地方
   w_mepc((uint64)main);

  // disable paging for now.
  // supervisor address translation and protection register
  // 不允许地址翻译 supervisor address translate and protection register
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  // 这两个我还不太懂，看注释是设置所有的中断，异常都会在supervisor模式下执行，
  // 根据前面设置的mpp位为1，也就是mret之后特权级会变为supervisor，
  // 最终肯定会进入usermode，此时发生中断、异常的话，也就会陷入内核的supervisor模式
  // machine exception delegation register.
  // machine interrupt delegation register.
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  // enable enternal, timer, software interrupt.
  // 允许supervisor模式下的所有异常。外部、定时器、软件、中断
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // ask for clock interrupts.
  // 定时器初始化
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  // 把当前的hartid写入tp寄存器
  int id = r_mhartid();
  w_tp(id);

  // switch to supervisor mode and jump to main().
  // 根据上面设置的mepc寄存器为main，这儿的mret将会把pc恢复到mepc，也就是跳转到main函数处执行
  // 同样的，上面设置了mstatus寄存器的mpp位为supervisor；在mret之后，系统的特权级别就会恢复到supervisor模式
  // 然后mpp会被设置为0，也就是用户模式 user mode.
  asm volatile("mret");
}

// set up to receive timer interrupts in machine mode,
// which arrive at timervec in kernelvec.S,
// which turns them into software interrupts for
// devintr() in trap.c.
void
timerinit()
{
  // each CPU has a separate source of timer interrupts.
  int id = r_mhartid();

  // ask the CLINT for a timer interrupt.
  int interval = 1000000; // cycles; about 1/10th second in qemu.
  // rv每次上电启动，计时器的寄存器mmtime会清零，在这儿我们设置timecmp为mtime + interval,
  // mtime跟随晶振每次自增，大于mtimecmp之后，触发定时器中断
  *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + interval;

  // prepare information in scratch[] for timervec.
  // scratch[0..3] : space for timervec to save registers.
  // scratch[4] : address of CLINT MTIMECMP register.
  // scratch[5] : desired interval (in cycles) between timer interrupts.
  // init each hart's mscratch register, thus can protection hart's context here.
  uint64 *scratch = &mscratch0[32 * id];
  scratch[4] = CLINT_MTIMECMP(id);
  scratch[5] = interval;
  // 设置每个hart用于保存上下文寄存器的地址,scratch数组，保存32个通用寄存器
  w_mscratch((uint64)scratch);

  // set the machine-mode trap handler.
  // 这儿设置了machine模式下的trap handler，为什么只有timervec呢，也就是说machine模式下只会有定时器的trap吗
  w_mtvec((uint64)timervec);

  // enable machine-mode interrupts.
  w_mstatus(r_mstatus() | MSTATUS_MIE);

  // enable machine-mode timer interrupts.
  w_mie(r_mie() | MIE_MTIE);
}
