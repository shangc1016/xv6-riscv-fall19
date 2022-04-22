#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"  
#include "kernel/fs.h"


// 参考grep.c 的部分， 关于正则表达式
// 参考ls.c 的部分，遍历目录


char*
fmtname(char *path)
{
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if(strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  // 重用了ls中的函数fmtname，这儿改成了填充'\0'
  memset(buf+strlen(p), '\0', DIRSIZ-strlen(p));
  return buf;
}

void find(char *path, const char *filename){
    int fd;
    char buf[512], *p;
    struct stat st;
    struct dirent de;  //目录文件中的一条目录项
    char *exact_name;
    

    if((fd = open(path, 0)) < 0){
        fprintf(2, "open can not open %s\n", path);
        return;
    }
    // 判断fd的状态。
    if(fstat(fd, &st) < 0){
        fprintf(2, "can not stat\n");
        return;
    }

    // printf("path:%s\n", path);
    switch(st.type){
        case T_FILE:
            exact_name = fmtname(path);
            if(!strcmp(exact_name, filename)){
                printf("%s\n", path);
            }
            break;
        case T_DIR:
            // 如果是文件、打开文件，读取dentry
            strcpy(buf, path);
            p = buf + strlen(buf);
            *p++ = '/';
            // 读目录文件里面的目录项dir entry
            while(read(fd, &de, sizeof(de)) == sizeof(de)){
                // 成功读到一条dir entry
                if(de.inum == 0){
                    continue;
                }
                if(!strcmp(de.name, ".") || !strcmp(de.name, "..")){
                    continue;
                }
                memmove(p, de.name, DIRSIZ);
                p[DIRSIZ] = 0;
                if(stat(buf, &st) < 0){
                    fprintf(2, "can stat dir %s\n", buf);
                }
                find(buf, filename);
            }
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