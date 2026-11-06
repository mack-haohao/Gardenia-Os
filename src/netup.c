#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <errno.h>

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct ifreq ifr;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "10.0.2.15", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) perror("SIOCSIFADDR");

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) perror("SIOCSIFNETMASK");

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) perror("SIOCGIFFLAGS");
    ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) perror("SIOCSIFFLAGS");

    struct rtentry route;
    memset(&route, 0, sizeof(route));
    struct sockaddr_in *addr;
    
    addr = (struct sockaddr_in *)&route.rt_gateway;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, "10.0.2.2", &addr->sin_addr);

    addr = (struct sockaddr_in *)&route.rt_dst;
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = INADDR_ANY;

    addr = (struct sockaddr_in *)&route.rt_genmask;
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = INADDR_ANY;

    route.rt_flags = RTF_UP | RTF_GATEWAY;
    route.rt_dev = "eth0";
    if (ioctl(fd, SIOCADDRT, &route) < 0) perror("SIOCADDRT");

    //printf("[netup] eth0 configured: 10.0.2.15/24, gw: 10.0.2.2\n");
    close(fd);
    return 0;
}
