#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  struct proc *p = myproc();

  // 增加之后的地址比MAXVA大，出错
  if(n + p->sz > MAXVA){
      goto ret;
  }
  if(n > 0){
      // 扩张内存，lazy分配
      p->sz += n;
  } else if(n < 0){
      // 收缩内存，直接解除映射，删除物理页面
      uint sz = p->sz;
      uint64 sp = p->tf->sp;
      // 收缩的比栈小，出错
      if(sz + n < sp) {
          goto ret;
      }
      // 解除映射
      sz = uvmdealloc(p->pagetable, sz, sz + n);
      p->sz = sz;
  }

//   if(growproc(n) < 0)
//     return -1;
ret:
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
