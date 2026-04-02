#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ipc.h"
#include "riscv_barrier.h"

static struct quard_star_ipc_shared *quard_star_ipc_shared_area =
    (struct quard_star_ipc_shared *)QUARD_STAR_IPC_SHM_BASE;

static void quard_star_ipc_ring_copy_in(struct quard_star_ipc_ring *ring,
                                        unsigned int pos,
                                        const unsigned char *src,
                                        unsigned int len)
{
    unsigned int first = len;

    if (pos + len > ring->size)
        first = ring->size - pos;

    memcpy(&ring->data[pos], src, first);
    if (len > first)
        memcpy(&ring->data[0], src + first, len - first);
}

static void quard_star_ipc_ring_copy_out(const struct quard_star_ipc_ring *ring,
                                         unsigned int pos,
                                         unsigned char *dst,
                                         unsigned int len)
{
    unsigned int first = len;

    if (pos + len > ring->size)
        first = ring->size - pos;

    memcpy(dst, &ring->data[pos], first);
    if (len > first)
        memcpy(dst + first, &ring->data[0], len - first);
}

static void quard_star_ipc_init_ring(struct quard_star_ipc_ring *ring)
{
    ring->prod = 0;
    ring->cons = 0;
    ring->size = QUARD_STAR_IPC_RING_SIZE;
    ring->dropped = 0;
}

void quard_star_ipc_init(void)
{
    struct quard_star_ipc_shared *shared = quard_star_ipc_shared_area;
    void *shared_base = (void *)(uintptr_t)shared;

    if (shared->magic == QUARD_STAR_IPC_MAGIC &&
        shared->version == QUARD_STAR_IPC_VERSION &&
        shared->linux_to_rtos.size == QUARD_STAR_IPC_RING_SIZE &&
        shared->rtos_to_linux.size == QUARD_STAR_IPC_RING_SIZE)
        return;

    memset(shared_base, 0, sizeof(*shared));
    quard_star_ipc_init_ring(&shared->linux_to_rtos);
    quard_star_ipc_init_ring(&shared->rtos_to_linux);
    smp_wmb();
    shared->version = QUARD_STAR_IPC_VERSION;
    __smp_store_release(&shared->magic, QUARD_STAR_IPC_MAGIC);
}

static int quard_star_ipc_push(struct quard_star_ipc_ring *ring,
                               volatile unsigned int *notify_pending,
                               const char *buf,
                               size_t len)
{
    unsigned int msg_len = (unsigned int)len;
    unsigned int prod;
    unsigned int cons;
    unsigned int used;
    unsigned int need;
    unsigned int pos;

    if (!buf || !len)
        return QUARD_STAR_IPC_ERR_INVAL;

    if (len > QUARD_STAR_IPC_MAX_MSG)
        return QUARD_STAR_IPC_ERR_TOOLONG;

    prod = ring->prod;
    cons = __smp_load_acquire(&ring->cons);
    used = prod - cons;
    need = (unsigned int)sizeof(msg_len) + msg_len;

    if (need > ring->size - used) {
        ring->dropped++;
        return QUARD_STAR_IPC_ERR_NOSPC;
    }

    pos = prod % ring->size;
    quard_star_ipc_ring_copy_in(ring, pos,
                                (const unsigned char *)&msg_len,
                                sizeof(msg_len));
    pos = (prod + sizeof(msg_len)) % ring->size;
    quard_star_ipc_ring_copy_in(ring, pos, (const unsigned char *)buf, msg_len);

    smp_wmb();
    __smp_store_release(&ring->prod, prod + need);
    __smp_store_release(notify_pending, 1U);

    return (int)msg_len;
}

static int quard_star_ipc_pop(struct quard_star_ipc_ring *ring,
                              volatile unsigned int *notify_pending,
                              char *buf,
                              size_t cap)
{
    unsigned int prod;
    unsigned int cons;
    unsigned int avail;
    unsigned int msg_len;
    unsigned int pos;

    if (!buf || !cap)
        return QUARD_STAR_IPC_ERR_INVAL;

    prod = __smp_load_acquire(&ring->prod);
    cons = ring->cons;
    avail = prod - cons;
    if (avail < sizeof(msg_len))
        return QUARD_STAR_IPC_ERR_EMPTY;

    pos = cons % ring->size;
    quard_star_ipc_ring_copy_out(ring, pos, (unsigned char *)&msg_len,
                                 sizeof(msg_len));

    if (!msg_len || msg_len > QUARD_STAR_IPC_MAX_MSG) {
        __smp_store_release(&ring->cons, prod);
        __smp_store_release(notify_pending, 0U);
        return QUARD_STAR_IPC_ERR_INVAL;
    }

    if (avail < sizeof(msg_len) + msg_len)
        return QUARD_STAR_IPC_ERR_EMPTY;

    if (msg_len > cap)
        return QUARD_STAR_IPC_ERR_TOOLONG;

    pos = (cons + sizeof(msg_len)) % ring->size;
    quard_star_ipc_ring_copy_out(ring, pos, (unsigned char *)buf, msg_len);

    smp_rmb();
    __smp_store_release(&ring->cons, cons + sizeof(msg_len) + msg_len);
    if (__smp_load_acquire(&ring->prod) == cons + sizeof(msg_len) + msg_len)
        __smp_store_release(notify_pending, 0U);

    return (int)msg_len;
}

int quard_star_ipc_send(const char *buf, size_t len)
{
    return quard_star_ipc_push(&quard_star_ipc_shared_area->rtos_to_linux,
                               &quard_star_ipc_shared_area->notify_pending_r2l,
                               buf, len);
}

int quard_star_ipc_recv(char *buf, size_t cap)
{
    return quard_star_ipc_pop(&quard_star_ipc_shared_area->linux_to_rtos,
                              &quard_star_ipc_shared_area->notify_pending_l2r,
                              buf, cap);
}
