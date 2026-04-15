#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#define CONTAINER_ID_LEN 32

struct monitor_request {
    int pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[CONTAINER_ID_LEN];
};

#define MONITOR_REGISTER 1
#define MONITOR_UNREGISTER 2

#endif
