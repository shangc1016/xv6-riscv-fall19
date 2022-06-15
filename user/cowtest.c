//
// tests for copy-on-write fork() assignment.
//

#include "kernel/types.h"
#include "kernel/memlayout.h"
#include "user/user.h"

// allocate more than half of physical memory,
// then fork. this will fail in the default
// kernel, which does not support copy-on-write.
void
simpletest()
{
  uint64 phys_size = PHYSTOP - KERNBASE;
  int sz = (phys_size / 3) * 2;
  // 这个sz已经超过物理内存的一半了，正常情况下fork之后，子进程完全拷贝福及才能拿的页表，然后kalloc，导致物理内存耗尽，然后killed

  printf("simple: ");
  
  char *p = sbrk(sz);
  if(p == (char*)0xffffffffffffffffL){
    printf("sbrk(%d) failed\n", sz);
    exit(-1);
  }

  for(char *q = p; q < p + sz; q += 4096){
    *(int*)q = getpid();
  }

  int pid = fork();
  if(pid < 0){
    printf("fork() failed\n");
    exit(-1);
  }

  if(pid == 0)
    exit(0);

  wait(0);

  if(sbrk(-sz) == (char*)0xffffffffffffffffL){
    printf("sbrk(-%d) failed\n", sz);
    exit(-1);
  }

  printf("ok\n");
}

// three processes all write COW memory.
// this causes more than half of physical memory
// to be allocated, so it also checks whether
// copied pages are freed.
void
threetest()
{
  uint64 phys_size = PHYSTOP - KERNBASE;
  int sz = phys_size / 4;
  int pid1, pid2;

  printf("three: ");
  
  char *p = sbrk(sz);
  if(p == (char*)0xffffffffffffffffL){
    printf("sbrk(%d) failed\n", sz);
    exit(-1);
  }

  pid1 = fork();
  if(pid1 < 0){
    printf("fork failed\n");
    exit(-1);
  }
  if(pid1 == 0){
    pid2 = fork();
    if(pid2 < 0){
      printf("fork failed");
      exit(-1);
    }
    if(pid2 == 0){
      for(char *q = p; q < p + (sz/5)*4; q += 4096){
        *(int*)q = getpid();
      }
      for(char *q = p; q < p + (sz/5)*4; q += 4096){
        if(*(int*)q != getpid()){
          printf("wrong content\n");
          exit(-1);
        }
      }
      exit(-1);
    }
    for(char *q = p; q < p + (sz/2); q += 4096){
      *(int*)q = 9999;
    }
    exit(0);
  }

  for(char *q = p; q < p + sz; q += 4096){
    *(int*)q = getpid();
  }

  wait(0);

  sleep(1);

  for(char *q = p; q < p + sz; q += 4096){
    if(*(int*)q != getpid()){
      printf("wrong content\n");
      exit(-1);
    }
  }

  if(sbrk(-sz) == (char*)0xffffffffffffffffL){
    printf("sbrk(-%d) failed\n", sz);
    exit(-1);
  }

  printf("ok\n");
}

char junk1[4096];
int fds[2];
char junk2[4096];
char buf[4096];
char junk3[4096];

// test whether copyout() simulates COW faults.
void
filetest()
{
  printf("file: ");
  
  buf[0] = 99;

  for(int i = 0; i < 4; i++){
    // 创建管道，这个是在内核kalloc一页内存，然后通过文件描述符关联到一起，不涉及COW机制
    if(pipe(fds) != 0){
      printf("pipe() failed\n");
      exit(-1);
    }
    // printf("fds[0] = %d; fds[1] = %d\n", fds[0], fds[1]);
    // printf("fds[0] addr = %p\n", &fds[0]);
    // printf("buf[0] addr = %p\n", &buf[0]);

    // fork了四个子进程，
    // 每次fork的时候，就会把当前进程的页表全部设置为COW，（通过pte中的rsw和去掉PTE_W）
    // printf("==before fork\n");
    int pid = fork();
    if(pid < 0){
      printf("fork failed\n");
      exit(-1);
    }
    if(pid == 0){
      // 四个子进程读管道
      sleep(1);
      // 把数据读入到buf中，就是写buf地址，此时，子进程会发生page fault，因为buf这个地址是和父进程通过COW机制复用的
    //   printf("== buf = %p\n", buf);
    //   printf("buf = %s\n", buf);
    //   printf("child fds[0] = %d\n", fds[0]);
    //   printf("child fds[0] addr = %p\n", &fds[0]);
    //   printf("child buf[0] addr = %p\n", &buf[0]);
      if(read(fds[0], buf, sizeof(i)) != sizeof(i)){
        printf("error: read failed\n");
        exit(1);
      }
      sleep(1);
      int j = *(int*)buf;
    //   printf("j = %d\n", j);
    //   printf("i = %d\n", i);
      if(j != i){
        printf("error: read the wrong value\n");
        exit(1);
      }
      exit(0);
    }
    // printf("==after fork\n");
    // printf("buf addr = %p\n", buf);
    // printf("== buf = %d\n", *(int*)buf);
    // 父进程写管道，这儿不会有写内存，也就不会有page fault

    if(write(fds[1], &i, sizeof(i)) != sizeof(i)){
      printf("error: write failed\n");
      exit(-1);
    }
    // close(fds[0]);
    // close(fds[1]);
  }

  int xstatus = 0;
  for(int i = 0; i < 4; i++) {
    wait(&xstatus);
    if(xstatus != 0) {
      exit(1);
    }
  }
  
  if(buf[0] != 99){
    printf("error: child overwrote parent\n");
    printf("buf[0] = %d\n", buf[0]);
    exit(1);
  }

  printf("ok\n");
}

int
main(int argc, char *argv[])
{
  simpletest();

//   check that the first simpletest() freed the physical memory.
  simpletest();

  threetest();
  threetest();
  threetest();

  filetest();

  printf("ALL COW TESTS PASSED\n");

  exit(0);
}
