#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/reboot.h>

static void spawn_service(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
        }
        execl(path, path, NULL);
        _exit(127);
    }
    printf("[Gardenia] started background service: %s (pid=%d)\n", path, pid);
}

static void do_mount_full(const char *src, const char *target,
                          const char *fstype, unsigned long flags, const char *data)
{
    if (mount(src, target, fstype, flags, data) < 0) {
        if (errno == EBUSY)
            printf("[Gardenia] %s already mounted, skip\n", target);
        else
            printf("[Gardenia] mount %s failed: %s\n", target, strerror(errno));
    } else {
        printf("[Gardenia] mounted %s (%s)\n", target, fstype);
    }
    fflush(stdout);
}

static void do_mount(const char *src, const char *target,
                     const char *fstype, unsigned long flags)
{
    do_mount_full(src, target, fstype, flags, NULL);
}

static void setup_stdio(void)
{
    int fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        return;
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
}

static void power_signal(int sig)
{
    sync();
    int action = (sig == SIGINT) ? RB_AUTOBOOT : RB_POWER_OFF;
    if (reboot(action) < 0) {
        const char msg[] = "[Gardenia] reboot failed\n";
        write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    }
}

static void setup_signals(void)
{
    struct sigaction sa = {
        .sa_handler = power_signal,
        .sa_flags   = SA_RESTART,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
}

static void spawn_shell(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("[Gardenia] fork");
        sleep(1);
        return;
    }

    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);

        setsid();
        int fd = open("/dev/console", O_RDWR | O_NOCTTY);
        if (fd >= 0) {
            ioctl(fd, TIOCSCTTY, 0);
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }

        execl("/bin/gash", "gash", NULL);
        _exit(127);
    }

    printf("[Gardenia] spawned shell, pid=%d\n", pid);
    fflush(stdout);

    int status;
    for (;;) {
        pid_t r = waitpid(-1, &status, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            printf("[Gardenia] waitpid: %s\n", strerror(errno));
            break;
        }
        if (r == pid) {
            if (WIFEXITED(status))
                printf("[Gardenia] shell exited, status=%d\n", WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                printf("[Gardenia] shell killed by signal %d\n", WTERMSIG(status));
            while (waitpid(-1, NULL, WNOHANG) > 0)
                ;
            break;
        }
    }
    fflush(stdout);
}

int main(void)
{
    printf("[Gardenia] System Init : hello, this is PID %d\n", getpid());
    printf("[Gardenia] System Init : mounting pseudo filesystems...\n");
    fflush(stdout);

    do_mount("devtmpfs", "/dev",  "devtmpfs", MS_NOSUID);
    setup_stdio();
    
    mkdir("/dev/pts", 0755);
    do_mount_full("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "ptmxmode=0666,mode=0620");

    do_mount("proc",     "/proc", "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC);
    do_mount("sysfs",    "/sys",  "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC);

    mkdir("/tmp", 01777);
    do_mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV);

    setup_signals();
    printf("[Gardenia] System Init : bringing up local network...\n");
    spawn_service("/bin/ifup");
    sleep(1);

    printf("[Gardenia] System Init : bringing up network...\n");
    spawn_service("/bin/netup");
    sleep(1);

    printf("[Gardenia] System Init : starting Gardenia API...\n");
    spawn_service("/bin/gardeniaApi");
    sleep(1);
    printf("[Gardenia] System Init : entering respawn loop\n");
    fflush(stdout);

    for (;;) {
        spawn_shell();
        sleep(1);
    }
}
