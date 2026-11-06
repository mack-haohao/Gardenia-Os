#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    if (kill(1, SIGTERM) < 0) {
        perror("shutdown");
        return 1;
    }
    return 0;
}
