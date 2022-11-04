#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"

int
main()
{
  char buf[BSIZE];
  int fd, i, blocks;

  fd = open("big.file", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf("bigfile: cannot open big.file for writing\n");
    exit(-1);
  }

  blocks = 0;
  // 每次写文件大小是sizeof(buf)，1024B，就是一个block的大小
  // 直到写满文件
  while(1){
    *(int*)buf = blocks;
    int cc = write(fd, buf, sizeof(buf));
    if(cc <= 0)
      break;
    blocks++;
    if (blocks % 100 == 0)
      printf(".");
  }

  printf("\nwrote %d blocks\n", blocks);
  // 关键的判断在这儿，最开始xv6的文件只有256+12=268个block
  // 要达到65803个block，需要更深的inode索引。
  if(blocks != 65803) {
    printf("bigfile: file is too small\n");
    exit(-1);
  }
  
  close(fd);
  fd = open("big.file", O_RDONLY);
  if(fd < 0){
    printf("bigfile: cannot re-open big.file for reading\n");
    exit(-1);
  }
  for(i = 0; i < blocks; i++){
    int cc = read(fd, buf, sizeof(buf));
    if(cc <= 0){
      printf("bigfile: read error at block %d\n", i);
      exit(-1);
    }
    if(*(int*)buf != i){
      printf("bigfile: read the wrong data (%d) for block %d\n",
             *(int*)buf, i);
      exit(-1);
    }
  }

  printf("bigfile done; ok\n"); 

  exit(0);
}
