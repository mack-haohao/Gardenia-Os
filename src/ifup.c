#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <errno.h>

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        perror("[Gardenia] : ioctl SIOCGIFFLAGS");
        close(fd);
        return 1;
    }

    ifr.ifr_flags |= IFF_UP;

    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        perror("[Gardenia] : ioctl SIOCSIFFLAGS");
        close(fd);
        return 1;
    }

    //printf("[Gardenia] : lo: interface up\n");
    close(fd);
    return 0;
}
