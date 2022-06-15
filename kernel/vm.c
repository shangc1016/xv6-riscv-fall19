#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

void print(pagetable_t);

// page reference count start
extern uint64 PGREFSTART;
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
    return 0;
    // panic("walk");

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


//// ========== LAB COW ======
// pte_t *cow_pte(pagetable_t pagetable, uint64 va, int alloc){
//     return walk(pagetable, va, alloc);
// }

// 父子进程共享页表，这个函数真正给子进程分配独立的物理页面，并且重新设置va->pa的映射
//  能进到这个函数，说明是因为COW的原因，那么进程的va地址对应的应该是COW
int cowalloc(pagetable_t pagetable, uint64 va){
    uint64 pa;
    uint flags;
    // printf("===enter cowalloc, va = %p\n", va);
    pte_t *pte = walk(pagetable, va, 0);
    if(pte == 0){
        return -1;
    }
    pa = PTE2PA(*pte);
    // printf("ppte = %p\n", *pte);
    // printf("pa = %p\n", pa);
    uint64 refcount = PGREFSTART + (pa - PGREFSTART) / PGSIZE;
    // printf("refcount = %d\n", *(uint8*)refcount);
    
    flags = PTE_FLAGS(*pte);
    
    // 既然已经是COW了，那就没有PTE_W、同时RSW位是有效的
    // 如果没有RSW标志位、同时还有PTE_W标志位。就说明这个不是COW的进程地址空间的地址，然后直接返回-1，出去之后被kill
    if((flags & PTE_W) || !(flags & PTE_RSW)){
        printf("==error\n");
        return -1;
    }
    flags += PTE_W;     // 加上PTE_W
    flags -= 0x100;     // 去掉rsw中的标志位，

    
    if(*(uint8*)refcount > 0){

        char *mem = (char*)kalloc();
        if(mem == 0){
            return -1;
        }
        memmove(mem, (void*)pa, PGSIZE);
        // 把refcount减一，
        // pte加上PTE_W标志位，去掉rsw中的标志位
        // 重新映射pte到mem
        uvmunmap(pagetable, va, PGSIZE, 1);
        mappages(pagetable, va, PGSIZE, (uint64)mem, flags);

    }else if(*(uint8*)refcount == 0){
        // 在pte中加上PTE_W的标志位，并且在rsw中去掉标志位，表示现在这个pte不再是cow机制了
        // printf("pte = %p\n", *pte);
        *pte = *pte & 0xfffffffffffffc00;  // pte把flags减掉，
        // printf("pte = %p\n", *pte);
        *pte += flags;                     // pte把新的flags加上
        // printf("pte = %p\n", *pte);
    }
    return 0;
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
// 此时的mappages还不能支持重复的映射，但是COW机制需要一个物理页面、
// 不对，这个的不能重复映射指的是，不能把什么内存映射到同一个进程虚拟地址上，和COW要涉及到的重复映射不一样
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va); // 起始va
  last = PGROUNDDOWN(va + size - 1);  // 终止va
  for(;;){
    // 根据va，找到对应的pte；这儿的1表示如果pte不存在，就分配相应的页表物理页面
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    // 检查PTE_V标记确保是首次映射
    if(*pte & PTE_V)
      panic("remap");
    // 如果是首次映射，那么就加上PTE_V标记
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
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0){
      printf("va=%p pte=%p\n", a, *pte);
      panic("uvmunmap: not mapped");
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
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
// 进程的sz由oldsz变成newsz
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  a = oldsz;
  // 从oldsz->newsz
  // 以页面为单位扩容
  for(; a < newsz; a += PGSIZE){
    // 先kalloc分配物理页面
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    // 然后初始化物理内存区域
    memset(mem, 0, PGSIZE);
    //最后映射到原来的oldsz处
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
      // 这个条件判断出了最后一级pte页表，最后一级不但有PTE_V标志位，还有PTE_W、PTE_X等标志位
      // 而中间页表只有PTE_V标记
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      // 这个就是中间页表，不是三级页表的最后一级
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, 0, sz, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
// 拷贝old页表到new页表，old页表的有map的大小是sz
// pagetable_t数据类型也就是uint64的指针
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  // sz是字节大小，页表是以PGSIZE为单位的
  for(i = 0; i < sz; i += PGSIZE){
    // walk函数找到第i个虚地址对应的物理地址，第三个参数为0表示如果这个PTE不存在，就不用kalloc实际分配了
    // 在sz以内的PTE应该都是映射过的，因此如果pte是0，表示没有映射，直接panic
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    // 如果这个pte没有PTE_V标记，表示这条pte无效，也panic
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    // 根据pte找到这一页的物理地址pa，这个pa是old页表一个pte映射的物理地址，
    pa = PTE2PA(*pte);

    flags = PTE_FLAGS(*pte);
    // 给flags去掉PTE_W，这个判断用来消除多次COW映射
    if(flags & PTE_W){
        flags -= PTE_W;
    }
    // 在flags中的rsw位置上加上引用次数
    if(!(flags & PTE_RSW)){
        flags += PTE_RSW;
    }
    *pte = *pte & 0xfffffffffffffc00;  // pte把原来的flags减掉，
    *pte += flags;                     // pte把新的flags加上
    
    // 映射到新页表的相同位置，mappages参数(新页表基地址，vm，sz，pa，flags)；
    // 在此处构造新的页表new的时候，使用的pte的标志位就是最新的标志位(已经去掉了PTE_W、并且在RSW位置已经增加了0x100)
    // 在这儿映射虚拟内存，使用的还是相同的物理地址；在影射之前把这块物理内存的引用数加一

    // refcount地址就是物理内存pa所对应的引用计数的地址
    uint64 refcount = PGREFSTART + (pa - PGREFSTART) / PGSIZE;
    // 然后对这个索引计数加一
    (*(uint8*)refcount)++;
    // 最后再map到新的页表中
    if(mappages(new, i, PGSIZE, pa, flags) != 0){
      return -1;
    }
  }
  return 0;
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
// 把数据从内核态拷贝到用户态
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    // 1、根据va得到页表中的pte
    pte_t *pte = walk(pagetable, va0, 0);
    if(pte == 0){
        return -1;
    }
    // 通过判断pte中的rsw位，判断这个地址是不是COW机制
    uint flags = PTE_FLAGS(*pte);
    if(flags & 0x300){
        cowalloc(pagetable, va0);
    }


    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
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
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
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
