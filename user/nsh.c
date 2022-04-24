#include"user/user.h"
#include"kernel/fcntl.h"
#include"kernel/param.h"

#define MAX_LEN 1024

#define ARG_SZ 128

char line[MAX_LEN];
char command[ARG_SZ][MAXARG];
char *cmd[MAXARG]; // MAXARG 是exec最大参数个数


void replace(char *str, int sz, char _old, char _new){
    char *p = str;
    for(int i=0; i<sz; i++){
        if(*p && *p == _old) *p = _new;
        p++;
    }
}

int substr(char *argv[], int argc, char *str){
    for(int i = 0; i < argc; i++){
        if(!strcmp(argv[i], str)) return i;
    }
    return -1;
}

void execute_redirection(int argc, char **argv){
    int in = substr(argv, argc, "<");
    int out = substr(argv, argc, ">");
    char *iname, *oname;
    int ifd = -1, ofd = -1;
    int pid = fork();
    if(pid == 0){
        if(in > -1){
            iname = argv[in + 1];
            argv[in] = 0;
            close(0);
            ifd = open(iname, O_RDONLY);
            if(ifd == -1){
                fprintf(2, "open error\n");
                exit(-1);
            }
        }
        if(out > -1){
            oname = argv[out + 1];
            argv[out] = 0;
            close(1);
            ofd = open(oname, O_CREATE | O_WRONLY);
            if(ofd == -1){
                fprintf(2, "open error\n");
                exit(-1);
            }
        }
        exec(argv[0], argv);
        fprintf(2, "exec error\n");
    }
    if(pid < 0){
        fprintf(2, "fork error\n");
        exit(-1);
    }
    if(pid > 0){
        wait(0);
    }
}


void execute_pipe(int argc, char **argv){
    int pipe_pos = substr(argv, argc, "|");
    if(pipe_pos == -1){
        execute_redirection(argc, argv);
    }else if(pipe_pos > 0){
        // 有管道
        argv[pipe_pos] = 0;
        int fd[2];
        // 创建管道
        if(pipe(fd) == -1){
            fprintf(2, "pipe error\n");
            exit(-1);
        }
        // fork两个进程，分别去执行管道两端的命令
        int first = fork();
        if(first == 0){
            close(1);
            dup(fd[1]);
            execute_redirection(pipe_pos, argv);
            close(fd[1]);
        }
        if(first < 0){
            fprintf(2, "fork error\n");
            exit(-1);
        }
        if(first > 0){
            int second = fork();
            if(second == 0){
                close(0);
                dup(fd[0]);
                execute_redirection(argc - pipe_pos, argv + pipe_pos + 1);
                close(fd[0]);
            }
            if(second < 0){
                fprintf(2, "fork error\n");
                exit(-1);
            }
            if(second > 0){
                // wait(&first);
                wait(0);
               
            }
        }
    }
}


void strncpy(char *dest, char *src, int sz){
    for(int i=0; i<sz; i++){
        dest[i] = src[i];
        i++;
    }
}

int main(int argc, char *argv[]){
    printf("@ ");
    memset(command, 0, sizeof(char) * ARG_SZ * MAXARG);
    int len=0, i=0;
    int sz;
    while(1){
        sz = read(0, &command[len][i], 1);
        if(command[len][i] == '\n') {
            command[len][i] = 0;
            len++;
            i = 0;
            continue;
        }
        if(sz == 0) break;
        i++;
    }
    for(int i = 0; i<len; i++){
        sz = strlen(command[i]);
        replace(command[i], sz, ' ', 0);
        char *p = command[i];
        int len = 0;
        cmd[len++] = p;
        for(int i = 0; i < sz; i++){
            if(*p == 0 && *(p+1) != 0) cmd[len++] = p+1;
            p++;
        }
        execute_pipe(len, cmd);
    }
    exit(0);
}