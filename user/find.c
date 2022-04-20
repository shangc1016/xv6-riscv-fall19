#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"  
#include "kernel/fs.h"


void find(char *path, const char *filename){
    int fd;
    struct stat st;
    struct dirent dt;
    

    if((fd = open(path, 0)) < 0){
        fprintf(2, "open can not open %s\n", path);
        return;
    }
    if(fstat(fd, &st) < 0){
        fprintf(2, "can not stat\n");
        return;
    }
    switch(st.type){
        case T_FILE:
            printf("file\n");
            break;
        case T_DIR:
            printf("dir: %s\n", path);
            // 如果是文件、

            // find(path, filename);
            break;
    }
    close(fd);
}


int main(int argc, char *argv[])
{
    if(argc != 3){
        fprintf(2, "usage: find something...\n");
        exit();
    }
    find(argv[1], argv[2]);
    exit();
}