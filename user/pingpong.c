#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"  // lib header file include syscall enter point, also use ulib.c/atoi()


// 使用fork，以及pipe。实现父子间通信



int main(int argc, char *argv[])
{
    int ping[2];
    int pong[2];

    if(pipe(ping) || pipe(pong)){
        fprintf(2, "init pipe error\n");
        exit();
    }

    int ret = fork();
    if(ret == 0){
        // child process;
        char child_ch;
        read(ping[0], &child_ch, 1);
        fprintf(1, "%d: received ping\n", getpid());
        write(pong[1], &child_ch,1);
    }
    if(ret > 0){
        char paretn_ch;
        // parent process:
        write(ping[1], "p", 1);
        read(pong[0], &paretn_ch, 1);
        fprintf(1, "%d: received pong\n", getpid());
    }
    if(ret < 0){
        // parent process, and fork error;
        fprintf(2, "fork error\n");
    }
    exit();
}
