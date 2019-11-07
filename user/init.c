// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = { "sh", 0 };

// initcode之后的第2个用户进程
// initcode的syscall参数是{"/init\0", 0}，其实也就是这儿的argc=1, argv只有一项，argv[0]="/init\0"
// 因为没有其他的参数，这儿main函数也就没有接收使用argvc，argv
int
main(void)
{
  int pid, wpid;

  // 下面的open和mknod都是syscall，open干了啥应该比较清楚，直接看mknod
  if(open("console", O_RDWR) < 0){
    // mknod也不太懂，反正就是如果没有console这个文件的话，就mknod创建一个设备，并且设置其major minor number.
    mknod("console", 1, 1);
    open("console", O_RDWR);
  }
  // 总之就是打开console设备，注意这是系统打开的第一个文件，所以它的fd是0
  // 这儿连续两次dup，复制fd=0的打开文件，这个实际上就是0，1，2三个fd实际上都指向了console设备
  dup(0);  // stdout
  dup(0);  // stderr

  // 死循环
  for(;;){
    printf("init: starting sh\n");
    // fork 生成子进程
    pid = fork();
    if(pid < 0){
      printf("init: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      // 然后在子进程中exec "sh"重新把sh这个ELF文件替换掉子进程的代码段等
      // 我们继续去/uyser/sh.c查看
      exec("sh", argv);
      printf("init: exec sh failed\n");
      exit(1);
    }
    while((wpid=wait(0)) >= 0 && wpid != pid){
      //printf("zombie!\n");
    }
  }
}
