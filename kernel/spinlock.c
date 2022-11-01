// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

#define NLOCK 1000

static int nlock;
static struct spinlock *locks[NLOCK];
// nlock 表示的是locks数组的长度
// nlocks 是在initlock的时候，设置的指针，指向某个spinlock；


// assumes locks are not freed
void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
  lk->nts = 0;
  lk->n = 0;
  if(nlock >= NLOCK)
    panic("initlock");

  // 在这儿更新locks指针数组
  // 这两行代码专门为了测试lock
  locks[nlock] = lk;
  nlock++;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void acquire(struct spinlock *lk) {
  push_off(); // disable interrupts to avoid deadlock.
  // 重复的acquire锁会panic
  if(holding(lk))
    panic("acquire");
  // 库函数，原子操作，增加lk->n
  __sync_fetch_and_add(&(lk->n), 1);

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  // 库函数，最终会调用amoswap原子交换
  while (__sync_lock_test_and_set(&lk->locked, 1) != 0) {
    // 这条语句也是在lock lab中用来统计lock数据的，
    // 记录acquire lock的时候自旋的次数
     __sync_fetch_and_add(&lk->nts, 1);
  }
  
  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  // 这个库函数是内存屏障，
  // 代码中的指令顺序和CPU执行的指令顺序不一定是相同的。
  // 加上内存屏障这一个指令，就可以保证在这条指令前的代码一定在其后的代码前执行。
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  // 实际上和给lk->locked赋值为0一样，但是普通的赋值(lk->locked=0)不是原子的
  // 这个指令同样会调用riscv的amoswap指令
  __sync_lock_release(&lk->locked);

  pop_off();
}

// Check whether this cpu is holding the lock.
int
holding(struct spinlock *lk)
{
  int r;
  push_off();
  r = (lk->locked && lk->cpu == mycpu());
  pop_off();
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  int old = intr_get();

  intr_off();
  // mycpu->noff==0：表示当前cpu不持有任何spinlock，
  // 调用push_off表示要acquire了，那么此时需要把当前的中断允许情况保存起来
  // 保存到mycpu()->intena中
  if(mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if(intr_get())
    panic("pop_off - interruptible");
  c->noff -= 1;
  if(c->noff < 0)
    panic("pop_off");
  if(c->noff == 0 && c->intena)
    intr_on();
}

void
print_lock(struct spinlock *lk)
{
  if (lk->n > 0)
    // 打印格式: 锁的名字，自旋次数nts，加锁次数n
    printf("lock: %s: #test-and-set %d #acquire() %d\n", lk->name, lk->nts, lk->n);
}


// lab lock 用到的syscall，统计lock的次数
uint64
sys_ntas(void)
{
  int zero = 0;
  int tot = 0;
  
  if (argint(0, &zero) < 0) {
    return -1;
  }
  if(zero == 0) {
    for(int i = 0; i < NLOCK; i++) {
      if(locks[i] == 0)
        break;
      locks[i]->nts = 0;
      locks[i]->n = 0;
    }
    return 0;
  }

  printf("=== lock kmem/bcache stats\n");
  for(int i = 0; i < NLOCK; i++) {
    if(locks[i] == 0)
      break;
    if(strncmp(locks[i]->name, "bcache", strlen("bcache")) == 0 ||
       strncmp(locks[i]->name, "kmem", strlen("kmem")) == 0) {
      tot += locks[i]->nts;
      print_lock(locks[i]);
    }
  }

  // 找到所有锁中top5 nts次数的锁
  printf("=== top 5 contended locks:\n");
  int last = 100000000;
  // stupid way to compute top 5 contended locks
  for(int t= 0; t < 5; t++) {
    int top = 0;
    for(int i = 0; i < NLOCK; i++) {
      if(locks[i] == 0)
        break;
      if(locks[i]->nts > locks[top]->nts && locks[i]->nts < last) {
        top = i;
      }
    }
    print_lock(locks[top]);
    last = locks[top]->nts;
  }
  return tot;
}
