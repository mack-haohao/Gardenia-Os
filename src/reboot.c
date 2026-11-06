#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    if (kill(1, SIGINT) < 0) {
        perror("reboot");
        return 1;
    }
    return 0;
}
