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

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;
// 全局变量kmem，表示物理内存

uint64 PGREFSTART;

void
kinit()
{
  // 先初始化锁
  initlock(&kmem.lock, "kmem");
  // 物理内存分配器管理内存范围是end到PHYSTOP
  // 因为要对每一个物理页面进行引用计数，所以在物理地址的起始，占用一部分内存空间，映射过来，
  // 1、把end地址对页面上取整，
  PGREFSTART = PGROUNDUP((uint64)end);
  // 2、计算物理内存有多少个页面
  uint64 pgcount = (PHYSTOP - PGREFSTART) / PGSIZE;
  // 3、因为每个页面使用一个字节表示，所以所有物理内存的引用所占用的内存就等于物理页面数量
  uint64 phystart = PGREFSTART + pgcount;
  // 4、最后把refstart到phystart这段内存初始化，清空为0
  memset((void*)PGREFSTART, 0, pgcount);


  freerange((void*)phystart, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  // 对这个区间中的每个物理页面进行free
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
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");


  uint64 refcount = PGREFSTART + ((uint64)pa - PGREFSTART) / PGSIZE;
  // 先得到物理页面地址对应的引用数，然后判断引用数，如果大于0，就减一，直接返回。如果等于0，就再真正释放物理内存
  if(*(uint8*)refcount > 0){
      (*(uint8*)refcount)--;
      return;
  }

  // Fill with junk to catch dangling refs.
  // 首先把这个页面全部写满1，
  memset(pa, 1, PGSIZE);
  // 然后把这个地址转换为链表指针
  r = (struct run*)pa;

  acquire(&kmem.lock);
  // 所有的空闲内存组成链表，链表采用头结点插入，r指向链表头
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  //分配物理内存，就是从r处去下一个链表节点，返回回去
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
