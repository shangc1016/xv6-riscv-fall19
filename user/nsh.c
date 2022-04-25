#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

#define MAX_LEN 1024

#define ARG_SZ 128

void replace(char *str, int sz, char _old, char _new) {
  char *p = str;
  for (int i = 0; i < sz; i++) {
    if (*p && *p == _old) *p = _new;
    p++;
  }
}

int substr(char *argv[], int argc, char *str) {
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], str)) return i;
  }
  return -1;
}

void execute_redirection(int argc, char **argv) {
    
//   fprintf(2, "%d\n", getpid());
//   for (int i = 0; i < argc; i++) fprintf(2, "argv:%s\n", argv[i]);
  int in = substr(argv, argc, "<");
  int out = substr(argv, argc, ">");
  char *iname, *oname;
  int ifd = -1, ofd = -1;
  if(in > -1){
    // 有输入重定向
    iname = argv[in + 1];
    argv[in] = 0;
    ifd = open(iname, O_RDONLY);
  }
  if(out > -1){
    oname = argv[out + 1];
    argv[out] = 0;
    ofd = open(oname, O_CREATE | O_WRONLY);
  }
  int pid = fork();
  if (pid == 0) {
    if (in > -1) {
      close(0);
      dup(ifd);
      // iname = argv[in + 1];
      // argv[in] = 0;
      // close(0);
      // ifd = open(iname, O_RDONLY);
      // if (ifd == -1) {
      //   fprintf(2, "open error\n");
      //   exit(-1);
      // }
    }
    if (out > -1) {
      close(1);
      dup(ofd);
      // oname = argv[out + 1];
      // argv[out] = 0;
      // close(1);
      // ofd = open(oname, O_CREATE | O_WRONLY);
      // if (ofd == -1) {
      //   fprintf(2, "open error\n");
      //   exit(-1);
      // }
    }
    // fprintf(2, "\n===pid:%d", getpid());
    // for (int i = 0; i < argc; i++) fprintf(2, "\nargv[%d]:%s", i, argv[i]);
    // fprintf(2, "\n");
    exec(argv[0], argv);
    fprintf(2, "\nexec error");
  }
  if (pid < 0) {
    fprintf(2, "fork error\n");
    exit(-1);
  }
  if (pid > 0) {
    wait(0);
    if(ifd > -1) close(ifd);
    if(ofd > -1) close(ofd);
  }
}

void execute_pipe(int argc, char **argv) {
    // fprintf(2, "\nexecute_pipe");
    // for(int i=0; i<argc; i++) fprintf(2, "\n$$$%s", argv[i]);
    // fprintf(2, "\n");
  int pipe_pos = substr(argv, argc, "|");
  if (pipe_pos == -1) {
    execute_redirection(argc, argv);
  } else if (pipe_pos > 0) {
    // for(int i=0; i<argc; i++) fprintf(2, "\n===%s", argv[i]);
    // 有管道
    argv[pipe_pos] = 0;
    int fd[2];
    // 创建管道
    if (pipe(fd) == -1) {
      fprintf(2, "pipe error\n");
      exit(-1);
    }
    // fork两个进程，分别去执行管道两端的命令
    int first = fork();
    if (first == 0) {
      close(1);
      dup(fd[1]);
      close(fd[0]);
      // for(int i=0; i < pipe_pos; i++) fprintf(2, "\n----first--argv:%s", argv[i]);
      execute_redirection(pipe_pos, argv);
      close(fd[1]);
    }
    if (first < 0) {
      fprintf(2, "fork error\n");
      exit(-1);
    }
    if (first > 0) {
      int second = fork();
      if (second == 0) {
        close(0);
        dup(fd[0]);
        close(fd[1]);

      // for(int i=0; i < argc - pipe_pos - 1; i++) fprintf(2, "\n----second--argv:%s", *(argv + pipe_pos + 1 + i));
        execute_redirection(argc - pipe_pos - 1, argv + pipe_pos + 1);
        close(fd[0]);
      }
      if (second < 0) {
        fprintf(2, "fork error\n");
        exit(-1);
      }
      if (second > 0) {
        wait(&second);
      }
    }
  }
}

void strncpy(char *dest, char *src, int sz) {
  for (int i = 0; i < sz; i++) {
    dest[i] = src[i];
    i++;
  }
}

char line[ARG_SZ] = {0};
char *cmd[MAXARG] = {0};  // MAXARG 是exec最大参数个数
char *p;

int sz;
int len;
int cmd_len;

int main(int argc, char *argv[]) {
  while (1) {
    printf("@ ");
    p = line;
    while (1) {
      sz = read(0, p, 1);
      // 命令以换行符分开，读到换行符说明读完了一条命令。
      if (sz == 0 || *p == '\n') break;
      p++;
    }
    if (sz == 0) break;
    len = p - line + 1;
    line[len - 1] = 0;
    // 一条命令的不同参数用空格分割，这儿把空格换成0；
    replace(line, len, ' ', 0);

    // for(int i=0; i< len; i++) fprintf(2, "\nline[%d]:%d,%c", i, line[i], line[i]);
    // fprintf(2, "\n");

    p = line;
    cmd_len = 0;
    if(*p) cmd[cmd_len++] = p;
    for (int i = 0; i < len - 1; i++) {
      if (*p == 0 && *(p + 1) != 0) cmd[cmd_len++] = p + 1;
      p++;
    }

    // for(int i=0; i<cmd_len; i++) fprintf(2, "\ncmd[%d]:%s", i, cmd[i]);
    // fprintf(2, "\n");
   
    execute_pipe(cmd_len, cmd);
    for(int i=0; i<ARG_SZ; i++) line[i] = 0;
  }
  exit(0);
}