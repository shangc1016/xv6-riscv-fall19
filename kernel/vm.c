#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

void print(pagetable_t);

/*
 * create a direct-map page table for the kernel and
 * turn on paging. called early, in supervisor mode.
 * the page allocator is already initialized.
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface 0
  kvmmap(VIRTION(0), VIRTION(0), PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface 1
  kvmmap(VIRTION(1), VIRTION(1), PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..39 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..12 -- 12 bits of byte offset within the page.
static pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kernel_pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove mappings from a page table. The mappings in
// the given range must exist. Optionally free the
// physical memory.
// 把虚拟空间地址从vm开始，大小为size的进程映射内存解除映射，Z这个区间中的进程页面必须都是有效的；
// 因为使用了lazy alloctor的方式，可能myproc()->sz =0x4008，但是只分配了4个页面，第五个页面还没分配，
// 但是这个函数，解除映射的区间是0x0 - 0x4008，所以他认为最后的第五个页表映射也是存在的，导致panic；
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 size, int do_free)
{
  uint64 a, last;
  pte_t *pte;
  uint64 pa;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 0)) == 0)
    // panic("uvmunmap: walk");
    // 解除映射这个函数默认在vm <--> vm + size 这个区间的所有虚地址都是有效的，
    // 但是采用lazy的方法导致虚地址空间有hole，
    // 遇到这种情况，就继续解除下一个pte的映射关系就行。
      goto done;
    if((*pte & PTE_V) == 0){
    // pte没有PTE_V标记，视为没有做映射
      goto done;

    //   printf("va=%p pte=%p\n", a, *pte);
    //   panic("uvmunmap: not mapped");
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
done:
    if(pte != 0) *pte = 0;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
}

// create an empty user page table.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    panic("uvmcreate: out of memory");
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  a = oldsz;
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  uint64 newup = PGROUNDUP(newsz);
  if(newup < PGROUNDUP(oldsz))
    uvmunmap(pagetable, newup, oldsz - newup, 1);

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
static void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      pagetable[i] = 0;
    //   panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  // 根据页表，解除映射关系，并且释放物理内存
  uvmunmap(pagetable, 0, sz, 1);
  // 释放页表元数据
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    // walk函数拿到第i个虚地址对应的pte，
    if((pte = walk(old, i, 0)) == 0)
    //   panic("uvmcopy: pte should exist");
      continue;
    // 一个进程所持有的pte必须是有效的，
    if((*pte & PTE_V) == 0)
      continue;
    //   panic("uvmcopy: page not present");
    //   continue;
    // 找到物理地址，以及标记位
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    // 为新的进程创建物理页面
    if((mem = kalloc()) == 0)
      goto err;
    // 复制物理页面的内容
    memmove(mem, (char*)pa, PGSIZE);
    // 做虚地址映射，mem映射到new页表的pte相同位置
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
// dstva就是read到的目的地，是用户进程空间的地址
// 对应read系统调用，数据从内核到用户空间
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  struct proc *p = myproc();
//   int flag = 0;
//   if(dstva == 0xefff) {
//       flag = 1;
//   }

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    // 首先，根据用户空间的虚拟地址进行地址翻译，得到物理地址
    pa0 = walkaddr(pagetable, va0);
    // 如果不存在这个物理地址，说明这个虚拟地址没有页表对应项，即没有分配物理内存。
    // 对于read偏移是进程sz-1的情况，pa0是有效的，
   
    if(pa0 == 0){
    
      // 如果是访问的进程虚拟地址大于进程的sz，说明访问地址越界，直接杀死进程
      if(dstva >= p->sz){
          p->killed = 1;
          return -1;
      }
     
      // 有可能是因为lazy机制，还未真正分配，那么就分配新的页面，并且映射到进程虚拟地址处；
      if(uvmlazyalloc(pagetable, va0) == -1){
          // uvmlazyalloc返回-1说明kalloc物理内存分配失败
          p->killed = 1;
          return -1;
      }
      // 重新做页表映射，这一次得到了有效的pa
      pa0 = walkaddr(pagetable, va0);
    }
    //   return -1;
    // dstva - va0 是目标地址在一页中的偏移
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
// srcva 是用户进程空间的地址
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  struct proc *p = myproc();

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0){
      if(va0 >= p->sz){
          p->killed = 1;
          return -1;
      }
      if(uvmlazyalloc(pagetable, va0) == -1){
          // uvmlazyalloc返回-1说明kalloc物理内存分配失败
          p->killed = 1;
          return -1;
      }
      // 重新做页表映射，这一次得到了有效的pa
      pa0 = walkaddr(pagetable, va0);
    }
    //   return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}




void vmprint_level(pagetable_t pagetable, int level) {
    for(int i = 0; i < 512; i++){
        pte_t pte = pagetable[i];
        // 前两级页表因为还有包括下一级也表地址，不能明确知道单个具体页表的权限，所以非叶子页表具有读写执行所有权限
        if((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            for(int j = 0; j < level; j++) printf(" ..");
            // pte有效，也表示个三级翻译机制，需要翻译三次
            // 根据pte得到物理地址，物理地址指向的地方又是一个pagetable
            uint64 child = PTE2PA(pte);
            printf(" ..%d: pte %p pa %p\n", i, pte, child);
            vmprint_level((pagetable_t)child, level + 1);
        } else if (pte & PTE_V) {
            // 叶子，走到页表的最后一级，也就是第三级了
            printf(" .. .. ..%d: pte %p pa %p\n", i, pte, PTE2PA(pte));
        }
    }
}   

// 打印进程的页表映射
void vmprint(pagetable_t pagetable) {
    vmprint_level(pagetable, 0);
    // 为啥打印的结果pte和pa这么相似呢，因为都是64位的。但具体含义不同。
    // pa是物理地址；pte是地址转换映射表，不是地址；
    // PTE_V 这个标记位标记这个pte是不是有效的，这个意思就是说这个pte是否包含有效的地址转换
    // 前两级page table的标志位是PTE_R、PTE_W、PTE_X全部都有的
}

// 在这之前要先判断能不能分配
// 这个函数在lazy的方式下真正的分配内存
int uvmlazyalloc(pagetable_t pagetable, uint64 vm){

    // 分配物理页面
    char  *mem = kalloc();
    if(mem == 0){
        return -1;
    }
    // 初始化物理页面
    memset(mem, 0, PGSIZE);

    vm = PGROUNDDOWN(vm);
    if(mappages(pagetable, vm, PGSIZE, (uint64)mem, PTE_U | PTE_W | PTE_R | PTE_X) != 0){
        kfree(mem);
        return -1;
    }
    return 0;
    // 返回值为-1，有三种情况，vm越界，物理内存耗尽。这种情况杀死进程
}
