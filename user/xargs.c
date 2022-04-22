#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"  
#include "kernel/fs.h"
#include "kernel/param.h"


/*

看xargstest.sh脚本的时候，以为xargs是执行每行指令的，
然后发现我理解的有问题，其实在运行这个程序的时候，如：
>./xargs args...
other args...
another args...
...
ctrl + D
解析全部的参数(args、other args、another arrgs...)，交个一个进程执行就行，其中新进程的命令为args中的第一个参数，其余参数都是新进程的参数。
*/



int main(int argc, char *argv[]){
    int sz;
    char buf[100]= {0}, *p;
    p = buf + 1; 
    while(1){
        sz = read(0, p, 1);
        if(sz == 0){
            break;
        } else if(sz < 0){
            printf("error\n");
            exit();
        }
        p++;
    }
    
    // sz = strlen(buf);
    sz = 100;
    // printf("sz:%d\n", sz);
    for(int i=0; i<sz;i++){
        if(buf[i] == ' ' || buf[i] == '\n') buf[i] = 0;
    }
    char *new_argv[MAXARG];
    int new_argc;
    for(int i=0; i< argc; i++){
        new_argv[i] = argv[i];
    }
    new_argc = argc;
    if(new_argc > MAXARG){
        printf("error\n");
        exit();
    }
    
    p = buf;
    // argv[argc++] = p;
    for(int i=0; i < sz-1; i++){
        if(*p == 0 && *(p+1) != 0){
            new_argv[new_argc++] = p+1;
            if(new_argc > MAXARG){
                printf("error\n");
                exit();
            }
        }
        p++;
    }
    
    int pid = fork();
    if(pid == 0){
        exec(new_argv[1], new_argv + 1);
    }
    if(pid < 0){
        printf("error\n");
    }
    if(pid > 0){
        wait();
    }
    exit();
}
