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
  // fprintf(2, "execute_redirection pid= %d\n", getpid());

  int in = substr(argv, argc, "<");
  int out = substr(argv, argc, ">");
  int pid;

  if ((pid = fork()) == 0) {
    // fprintf(2, "\nredirect pid=%d\n", getpid());
    if (in > -1) {
      close(0);
      if (open(argv[in + 1], O_RDONLY) != 0) {
        fprintf(2, "open read error\n");
        exit(-1);
      }
      argv[in] = 0;
    }
    if (out > -1) {
      close(1);
      if (open(argv[out + 1], O_CREATE | O_WRONLY) != 1) {
        fprintf(2, "open write error\n");
        exit(-1);
      }
      argv[out] = 0;
    }
    // for(int i=0; i< argc; i++){
    //   fprintf(2, "\n==%s", argv[i]);
    // }
    exec(argv[0], argv);
  }
  if (pid > 0) {
    wait(0);
  }
}

void execute_pipe(int argc, char **argv) {
  int pipe_pos = substr(argv, argc, "|");

  if (pipe_pos == -1) {
    execute_redirection(argc, argv);
  } else if (pipe_pos > 0) {
    argv[pipe_pos] = 0;
    int fd[2];
    int first, second;

    // create pipe
    if (pipe(fd) == -1) {
      fprintf(2, "pipe error\n");
      exit(-1);
    }

    if ((first = fork()) == 0) {
      // fprintf(2, "\nfirst pid=%d\n", getpid());
      close(1);
      dup(fd[1]);
      close(fd[0]);
      close(fd[1]);
      execute_redirection(pipe_pos, argv);
      exit(0);
    }

    if ((second = fork()) == 0) {
      // fprintf(2, "\nsecond pid=%d\n", getpid());
      close(0);
      dup(fd[0]);
      close(fd[0]);
      close(fd[1]);
      execute_redirection(argc - pipe_pos - 1, argv + pipe_pos + 1);
      exit(0);
    }

    wait(0);
    // wait(0);
    close(fd[0]);
    close(fd[1]);
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
      if (sz == 0 || *p == '\n') break;
      p++;
    }
    if (sz == 0) break;
    len = p - line + 1;
    line[len - 1] = 0;
    replace(line, len, ' ', 0);

    p = line;
    cmd_len = 0;
    if (*p) cmd[cmd_len++] = p;
    for (int i = 0; i < len - 1; i++) {
      if (*p == 0 && *(p + 1) != 0) cmd[cmd_len++] = p + 1;
      p++;
    }

    execute_pipe(cmd_len, cmd);
    memset(line, 0, sizeof(line));
  }
  exit(0);
}