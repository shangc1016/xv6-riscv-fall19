#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz, sp, ustack[MAXARG+1], stackbase;
  struct elfhdr elf;    // elf头
  struct inode *ip;     // 程序二进制文件的inode
  struct proghdr ph;    // elf文件中的program header
  pagetable_t pagetable = 0, oldpagetable;   // 进程页表
  struct proc *p = myproc();

  begin_op(ROOTDEV); 
 // 在这儿做了path到inode的转换
  if((ip = namei(path)) == 0){
    end_op(ROOTDEV);
    return -1;
  }
  ilock(ip);

  // Check ELF header
  // 首先在程序二进制文件最开始读一个elf header大小的数据到elfheader结构体中
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  // 判断魔数  
  if(elf.magic != ELF_MAGIC)
    goto bad;

  // 然后分配页表，这个页表里面没有用户进程特有的数据，只有trapframe和trampoline，用于系统调用的所有进程都一样的页表映射。
  // 这个页表还没有设置为当前进程的页表。
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  // 加载各个程序段到内存
  sz = 0; // 这儿的sz表示新的进程的大小
  // 依次把program header读入到ph结构体中
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    //   读文件的偏移最初有elf header给出，后面依次加上一个program header的长度
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    //   uvmalloc:(user virtual memory malloc)这个函数根据program header中每个段的虚地址做映射。
    // uvmalloc主要就是从内核分配空间，kalloc，然后memset清空，然后mappages映射到虚地址的不同地方。
    if((sz = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    //   uvmalloc已经分配好了虚地址空间了，然后就读二进制文件，把各个段放到虚地址对应的物理地址的各位置。
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op(ROOTDEV);
  ip = 0;
//   上面已经把新进程空间创建好了

  p = myproc();
  uint64 oldsz = p->sz;
  // 当前进程的内存大小
  // 这个oldsz是为了后面释放原来的页表准备的。

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  // 在当前进程前面分配两个页面，然后把第二个页面当做栈空间。
  // 设置sp，和stack_base
  sz = PGROUNDUP(sz);
  if((sz = uvmalloc(pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  uvmclear(pagetable, sz-2*PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;   
  

  // 然后最开始的参数argv按照顺序压到栈中。
  // argv是个二维数组，argc是参数长度
  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  // 设置进程的a1寄存器为sp，等到退出syscall的时候，从trapframe中回复寄存器的值，a1就是函数调用的参数
  p->tf->a1 = sp;


  // Save program name for debugging.
  // 设置进程的名字等信息用来debug
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  // 在这儿切换进程页表，以及sz，并且释放旧的页表
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  // epc在恢复寄存器的时候就是pc寄存器，函数中从这儿开始执行
  p->tf->epc = elf.entry;  // initial program counter = main
  // 初始化栈指针sp
  p->tf->sp = sp; // initial stack pointer
  // 释放旧页表
  proc_freepagetable(oldpagetable, oldsz);
  // 返回的是argc参数长度，syscall的返回值放在a0寄存器，a0寄存器刚好又是函数调用的参数，a1放的是argv参数
  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op(ROOTDEV);
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  if((va % PGSIZE) != 0)
    panic("loadseg: va must be page aligned");

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
