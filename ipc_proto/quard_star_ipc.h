#ifndef QUARD_STAR_IPC_PROTO_H
#define QUARD_STAR_IPC_PROTO_H

#define QUARD_STAR_IPC_MAGIC        0x51534950U
#define QUARD_STAR_IPC_VERSION      1U
#define QUARD_STAR_IPC_RING_SIZE    2048U
#define QUARD_STAR_IPC_MAX_MSG      256U
#define QUARD_STAR_IPC_SHM_BASE     0xBF7F0000UL
#define QUARD_STAR_IPC_SHM_SIZE     0x00010000UL

struct quard_star_ipc_ring {
    unsigned int prod;
    unsigned int cons;
    unsigned int size;
    unsigned int dropped;
    unsigned char data[QUARD_STAR_IPC_RING_SIZE];
};

struct quard_star_ipc_shared {
    unsigned int magic;
    unsigned int version;
    unsigned int features;
    unsigned int reserved0;
    unsigned int notify_pending_l2r;
    unsigned int notify_pending_r2l;
    unsigned int reserved1;
    unsigned int reserved2;
    struct quard_star_ipc_ring linux_to_rtos;
    struct quard_star_ipc_ring rtos_to_linux;
};

#endif
