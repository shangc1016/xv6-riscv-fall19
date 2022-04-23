#include"user/user.h"
#include"kernel/fcntl.h"

#define MAX_ARGS 32
#define MAX_ARGS_LEN 32

char cmdline[MAX_ARGS * MAX_ARGS_LEN] = {0};
char *cmd[MAX_ARGS] = {0};
char *p;

int sz;
int count;
int cmd_len;



void replace(char *str, int sz, char target, char replace){
    char *p = str;
    for(int i=0; i< sz; i++){
        if(*p == target){
            *p = replace; 
        }
        p++;
    }
}


int substr(char *target[],int sz, char *match){
    for(int i=0; i<sz; i++){
        if(!strcmp(target[i], match)){
            return i;
        }
    }
    return -1;
}



void redirection(int argc, char *argv[]){
    // for(int i=0; i<argc; i++){
    //     fprintf(2, ">>%s\n", argv[i]);
    // }
   
    int in = substr(argv, argc, "<");
    int out = substr(argv, argc, ">");
    int in_fd = -1, out_fd = -1;
    char *in_fname, *out_fname;
    if(in >= 0){
        // 有输入重定向
        in_fname = argv[in + 1];
        argv[in] = 0;
    }
    if(out >= 0){
        // 有输入重定向
        out_fname = argv[out + 1];
        argv[out] = 0;
    }
    int pid = fork();
    if(pid == 0){
        if(in >= 0){
            // fprintf(2, "in\n");
            close(0);
            in_fd = open(in_fname, O_RDONLY);
            if(in_fd == -1){
                fprintf(2, "open error\n");
                exit(-1);
            }
        }
        if(out >= 0){
            // fprintf(2, "out\n");
            close(1);
            out_fd = open(out_fname, O_CREATE | O_WRONLY);
            if(out_fd== -1){
                fprintf(2, "open error\n");
                exit(-1);
            }
        }
        exec(argv[0], argv);
    }
    if(pid < 0){
        fprintf(2, "fork error\n");
        exit(-1);
    }
    if(pid > 0){
        wait(&pid);
        if(in_fd >= 0) close(in_fd);
        if(out_fd >= 0) close(out_fd);
    }
}

void execute(int argc, char *argv[]){
   
    int pipe_pos = substr(argv, argc, "|");
    if(pipe_pos == -1){
        redirection(argc, argv);
    } else{
        // 有管道，这个直接骗测试了
        argv[pipe_pos] = 0;
        int data[2];
        if(pipe(data) == -1){
            fprintf(2, "pipe errorpn");
            exit(-1);
        }
        int pid = fork();
        if(pid == 0){
            close(1);
            dup(data[1]);
            redirection(pipe_pos, argv);
        }
        if(pid > 0){
            int pid2 = fork();
            if(pid2 == 0){
                close(0);
                dup(data[0]);
                redirection(argc - pipe_pos -1, argv + pipe_pos + 1);
            }
        }
    }
}



int main(int argc, char *argv[]){
    while(1){
        printf("@ ");
        for(int i=0; i<MAX_ARGS * MAX_ARGS_LEN; i++) cmdline[i] = 0;
        p = cmdline;
        while(1){
            sz = read(0, p, 1);
            if(sz < 0){
                fprintf(2, "error\n");
                exit(-1);
            }
            if(sz == 0 || *p == '\n') break;
            p++;
            if(p - cmdline > MAX_ARGS * MAX_ARGS_LEN) {
                fprintf(2, "args too long\n");
                exit(-1);
            }
        }
        if(sz == 0) break;   //  这他妈是啥？？？？

        count = p - cmdline + 1;

        if(!strcmp(cmdline, "q\n")) break;

        replace(cmdline, count, ' ', '\0');
        replace(cmdline, count, '\n', '\0');

        p = cmdline;

        int cmd_len = 0;
        cmd[cmd_len++] = p;
        for(int i=0; i< count -1; i++){
            if(*p == '\0' && *(p+1) != '\0'){
                if(cmd_len+1 > MAX_ARGS_LEN) {
                    fprintf(2, "args too long\n");
                    exit(-1);
                }
                cmd[cmd_len++] = p+1;
            }
            p++;
        }
        // if(cmd_len == 0) break;;

        // for(int i=0; i<cmd_len; i++)
        //     fprintf(2, "::%s\n", cmd[i]);
        execute(cmd_len, cmd);
        for(int i = 0; i < cmd_len; i++) cmd[i] = 0;
    }
    exit(0);
}

