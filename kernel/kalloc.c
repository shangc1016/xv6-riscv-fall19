// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// lab:lock
// struct {
//   struct spinlock lock;
//   struct run *freelist;
// } kmem;

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

// lab:lock end;


// 只有0号cpu执行kinit，初始化物理内存，初始化到当前CPU的list中
void
kinit()
{
  // lab:lock
  int id;
  push_off();
  id = cpuid();
  pop_off();

  // 1. 初始化kmem的锁
  initlock(&kmem[id].lock, "kmem");
  kmem[id].freelist = 0;
  // 2. 初始化物理内存，范围是从end到PHYSTOP;
  // 只让0号cpu初始化这儿的物理内存，这儿加上id==0的判断，
  // 方便让其他线程也调用初始化结构体，不会初始化物理内存
  if (id == 0) freerange(end, (void *)PHYSTOP);
  // lab:lock end
}

// 初始化内存区域
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  // 对在这个区域内的所有PAGE调用kfree
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  int id;
  struct run *r;
  // 检查传入的物理地址是否合法
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  // 在每个PAGE的起始位置，指针类型转为struct run
  r = (struct run *)pa;

  push_off();
  id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  // 然后把这些PAGE用链表连起来，新的PAGE插入到kmem.freelist的头部
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
}


struct run *steal_kmem(int cpuid) {
  struct run *r;
  for (int i = 0; i < NCPU; i++) {
    if (i == cpuid) continue;
    acquire(&kmem[i].lock);
    r = kmem[i].freelist;
    if(r) kmem[i].freelist = r->next;
    release(&kmem[i].lock);
    if (r) return r;
  }
  return (struct run*)0;
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  int id;

  // lab:lock start
  push_off();
  id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if (r) kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  // if(!r), steal from other cpu's kmem
  // if(!r)说明cpu从自己的kmem上没有找到空闲的内存，然后就需要从别的cpu的kmem上steal内存
  // 挨个遍历kmem数组，查找各自的freelist数组，从任何一个cpuid的kmem找到的话，直接返回。
  // 如果还是没有找到空闲内存的话，在kalloc中直接返回0，同样会在sys_exec中panic；
  if(!r) r = steal_kmem(id);
  

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
