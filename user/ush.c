#include"user/user.h"



char cmd[100], *p;
int sz;

int main(int argc, char *argv[]){
    
    while(1){
        p = cmd;
        sz = read(0, p, 1);
        if(sz < 0){
            fprintf(2, "error\n");
            exit(-1);
        }
    }
}