#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"  



void action(int left, int right, int num){
    int value;
    while(1){
        if(read(left, &value, sizeof(int)) == -1) break;
        if(value == num){
            fprintf(1, "prime %d\n", value);
        } else if (value % num == 0) continue;
        else{
            write(right, &value, sizeof(int));
        }
    }
}

int main()
{
    int pipe2[2];
    int pipe3[2];
    int pipe5[2];
    if(pipe(pipe2) || pipe(pipe3) || pipe(pipe5)){
        fprintf(2, "pipe init error\n");
        exit();
    }
    for(int i=2; i<35; i++)
        write(pipe2[1], &i, sizeof(int));
    
    int ret;
    if((ret = fork()) == 0){
        action(pipe2[0], pipe3[1], 2);
    }
    if(ret < 0){
        fprintf(2, "fork error\n");
        exit();
    }
    if(ret > 0){
        if((ret = fork()) == 0){
            action(pipe3[0], pipe5[1], 3);
        }
        if(ret < 0){
            fprintf(2, "fork error\n");
            exit();
        }
        if(ret > 0){
            if((ret = fork()) == 0){
                int val;
                while(1){
                    if(read(pipe5[0], &val, sizeof(val)) == -1) break;
                    fprintf(1, "prime %d\n", val);
                }
                exit();
            }
        }
    }
    exit();
}