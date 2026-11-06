#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; 
    (void)argv;
    
    char *name = ttyname(STDIN_FILENO);
    if (!name) {
        fprintf(stderr, "not a tty\n");
        return 1;
    }
    printf("%s\n", name);
    return 0;
}
