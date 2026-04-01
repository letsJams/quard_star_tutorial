#ifndef QUARD_STAR_IPC_PROTO_H
#define QUARD_STAR_IPC_PROTO_H

#define QUARD_STAR_IPC_MAGIC        0x51534950U
#define QUARD_STAR_IPC_VERSION      1U
#define QUARD_STAR_IPC_RING_SIZE    2048U
#define QUARD_STAR_IPC_MAX_MSG      256U
#define QUARD_STAR_IPC_SHM_BASE     0xBF7F0000UL
#define QUARD_STAR_IPC_SHM_SIZE     0x00010000UL

#define QUARD_STAR_IPC_MSG_MAGIC      0x514D5347U
#define QUARD_STAR_IPC_MSG_VERSION    1U

#define QUARD_STAR_IPC_MSG_TYPE_REQ   1U
#define QUARD_STAR_IPC_MSG_TYPE_RESP  2U
#define QUARD_STAR_IPC_MSG_TYPE_EVT   3U

#define QUARD_STAR_IPC_SERVICE_DEMOCHAR  1U
#define QUARD_STAR_IPC_SERVICE_LED       2U

#define QUARD_STAR_IPC_DEMOCHAR_READ     1U
#define QUARD_STAR_IPC_DEMOCHAR_WRITE    2U
#define QUARD_STAR_IPC_DEMOCHAR_VERSION  3U

#define QUARD_STAR_IPC_LED_GET           1U
#define QUARD_STAR_IPC_LED_SET           2U

#define QUARD_STAR_IPC_STATUS_OK       0
#define QUARD_STAR_IPC_STATUS_INVAL   -22
#define QUARD_STAR_IPC_STATUS_NOSYS   -38
#define QUARD_STAR_IPC_STATUS_IO      -5
#define QUARD_STAR_IPC_STATUS_TOOLONG -90

struct quard_star_ipc_msg_hdr {
    unsigned int magic;
    unsigned char version;
    unsigned char type;
    unsigned char service;
    unsigned char opcode;
    unsigned int seq;
    int status;
    unsigned int len;
};

struct quard_star_ipc_demochar_value {
    unsigned int value;
};

struct quard_star_ipc_led_state {
    unsigned int led;
    unsigned int value;
};

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
