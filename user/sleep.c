#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"  // lib header file include syscall enter point, also use ulib.c/atoi()

int main(int argc, char *argv[]){
    if (argc != 2){
        fprintf(2, "usage; sleep time\n");
        exit();
    }
    char *ch;
    for (ch = argv[1]; *ch != '\0'; ch++){
        if(!('0' <= *ch && *ch <= '9')){
            fprintf(2, "time should be int\n");
            exit();
        }
    }

    sleep(atoi(argv[1]));
    exit();
}