#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);


// 这个函数真的很长...
// 首先需要知道exec函数干了啥，有了大方向再看代码就清晰很多
// exec会把要执行的ELF文件加载到当前用户进程空间中，替换现有的进程
int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz, sp, ustack[MAXARG+1], stackbase;
  struct elfhdr elf;                                    // ELF文件头
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op(ROOTDEV);

  // 根据path解析得到inode
  if((ip = namei(path)) == 0){
    end_op(ROOTDEV);
    return -1;
  }
  ilock(ip);

  // Check ELF header
  // 首先读入elf header
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  // 检查魔数
  if(elf.magic != ELF_MAGIC)
    goto bad;

  // 为进程p创建页表，注意这个进程p其实就是initcode进程，它是有自己的页表的
  // 注意这个函数并不会和原来已经有的页表耦合，混合；其实对原来的页表根本没影响
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  sz = 0;

  // 遍历elf文件的所有program header，读到ph里面
  // elf里面有多个program header，它们的起始地址是phoff，个数是phnum，是按照数组顺序存放的
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    // 下面是对program header的一些validation
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    // uvmalloc实际上是让页表从old_sz增加到new_sz，增加的这些分配物理内存
    if((sz = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    // 然后根据program header，把program加载到uvmalloc申请到的地方
    // 总之就是从elf文件中，把program程序段读到页表的相应地址
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op(ROOTDEV);
  ip = 0;

  // 此时pagetable就是进程新的页表

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  // 这一步是设置好新的页表的栈空间，多分配两页，一页用作stack
  sz = PGROUNDUP(sz);
  if((sz = uvmalloc(pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  uvmclear(pagetable, sz-2*PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  // 把exec的参数数组初始化到栈空间内
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    // 关键就是这个，把argv[argc]拷贝到页表的sp处
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    // ustack是把每个参数的偏移量记下来
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  // 最后把argv这个数组的地址写入栈中
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  // exec创建出来的进程的argv参数的地址就是p->tf->a1，第一个参数
  p->tf->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  // 上面的循环，让last是path中/后的部分
  // 拷贝进程名
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  // 最后切换当前进程的页表，初始化sz，sp，epc等参数
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->tf->epc = elf.entry;  // initial program counter = main
  p->tf->sp = sp; // initial stack pointer
  // 最后释放原来的页表
  proc_freepagetable(oldpagetable, oldsz);
  // 最后返回的是argc，也就是exec的第一个参数，也就是exec参数的个数
  // 在syscall中，把这个值赋给a0寄存器，最后从内核重新进入用户态的时候，a0，a1参数分别是argc、argv
  // 然后epc是elf.entry，也就重新进入用户态进程的起始位置开始执行了
  // 在这儿，我们还是以initcode为例子，initcode的syscall的参数是{"/init\0"， 0}
  // 那就会调用根文件系统下的/init这个文件，加载到内存，替换成他的页表，返回到usermode执行
  // init对应的代码在user/init.c，至于xv6的文件系统是怎么一回事，先按下不表
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
