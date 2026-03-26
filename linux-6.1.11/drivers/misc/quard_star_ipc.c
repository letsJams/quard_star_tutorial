// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared-memory IPC between Linux and the cpu7 FreeRTOS trusted domain.
 * V1 keeps the data path minimal: two SPSC rings and polling from both sides.
 */

#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <quard_star_ipc.h>

struct quard_star_ipc {
    struct miscdevice miscdev;
    struct mutex lock;
    struct quard_star_ipc_shared *shared;
    phys_addr_t phys;
    size_t size;
};

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

static void quard_star_ipc_init_shared(struct quard_star_ipc *ipc)
{
    if (ipc->shared->magic == QUARD_STAR_IPC_MAGIC &&
        ipc->shared->version == QUARD_STAR_IPC_VERSION &&
        ipc->shared->linux_to_rtos.size == QUARD_STAR_IPC_RING_SIZE &&
        ipc->shared->rtos_to_linux.size == QUARD_STAR_IPC_RING_SIZE)
        return;

    memset(ipc->shared, 0, sizeof(*ipc->shared));
    quard_star_ipc_init_ring(&ipc->shared->linux_to_rtos);
    quard_star_ipc_init_ring(&ipc->shared->rtos_to_linux);
    smp_wmb();
    WRITE_ONCE(ipc->shared->version, QUARD_STAR_IPC_VERSION);
    __smp_store_release(&ipc->shared->magic, QUARD_STAR_IPC_MAGIC);
}

static int quard_star_ipc_push(struct quard_star_ipc_ring *ring,
                               unsigned int *notify_pending,
                               const unsigned char *buf,
                               size_t len)
{
    unsigned int msg_len = (unsigned int)len;
    unsigned int prod;
    unsigned int cons;
    unsigned int used;
    unsigned int need;
    unsigned int pos;

    if (!buf || !len)
        return -EINVAL;

    if (len > QUARD_STAR_IPC_MAX_MSG)
        return -EMSGSIZE;

    prod = READ_ONCE(ring->prod);
    cons = __smp_load_acquire(&ring->cons);
    used = prod - cons;
    need = (unsigned int)sizeof(msg_len) + msg_len;

    if (need > ring->size - used) {
        WRITE_ONCE(ring->dropped, READ_ONCE(ring->dropped) + 1);
        return -ENOSPC;
    }

    pos = prod % ring->size;
    quard_star_ipc_ring_copy_in(ring, pos, (const unsigned char *)&msg_len,
                                sizeof(msg_len));
    pos = (prod + sizeof(msg_len)) % ring->size;
    quard_star_ipc_ring_copy_in(ring, pos, buf, msg_len);

    smp_wmb();
    __smp_store_release(&ring->prod, prod + need);
    WRITE_ONCE(*notify_pending, 1U);

    return (int)msg_len;
}

static int quard_star_ipc_pop(struct quard_star_ipc_ring *ring,
                              unsigned int *notify_pending,
                              unsigned char *buf,
                              size_t cap)
{
    unsigned int prod;
    unsigned int cons;
    unsigned int avail;
    unsigned int msg_len;
    unsigned int pos;

    if (!buf || !cap)
        return -EINVAL;

    prod = __smp_load_acquire(&ring->prod);
    cons = READ_ONCE(ring->cons);
    avail = prod - cons;
    if (avail < sizeof(msg_len))
        return -EAGAIN;

    pos = cons % ring->size;
    quard_star_ipc_ring_copy_out(ring, pos, (unsigned char *)&msg_len,
                                 sizeof(msg_len));

    if (!msg_len || msg_len > QUARD_STAR_IPC_MAX_MSG) {
        __smp_store_release(&ring->cons, prod);
        WRITE_ONCE(*notify_pending, 0U);
        return -EIO;
    }

    if (avail < sizeof(msg_len) + msg_len)
        return -EAGAIN;

    if (msg_len > cap)
        return -EMSGSIZE;

    pos = (cons + sizeof(msg_len)) % ring->size;
    quard_star_ipc_ring_copy_out(ring, pos, buf, msg_len);

    smp_rmb();
    __smp_store_release(&ring->cons, cons + sizeof(msg_len) + msg_len);
    if (__smp_load_acquire(&ring->prod) == cons + sizeof(msg_len) + msg_len)
        WRITE_ONCE(*notify_pending, 0U);

    return (int)msg_len;
}

static ssize_t quard_star_ipc_read(struct file *file, char __user *buf,
                                   size_t len, loff_t *ppos)
{
    struct miscdevice *miscdev = file->private_data;
    struct quard_star_ipc *ipc = container_of(miscdev, struct quard_star_ipc,
                                              miscdev);
    unsigned char payload[QUARD_STAR_IPC_MAX_MSG];
    int ret;

    if (!len)
        return 0;

    mutex_lock(&ipc->lock);
    ret = quard_star_ipc_pop(&ipc->shared->rtos_to_linux,
                             &ipc->shared->notify_pending_r2l,
                             payload, len > QUARD_STAR_IPC_MAX_MSG ?
                             QUARD_STAR_IPC_MAX_MSG : len);
    mutex_unlock(&ipc->lock);
    if (ret < 0)
        return ret;

    if (copy_to_user(buf, payload, ret))
        return -EFAULT;

    return ret;
}

static ssize_t quard_star_ipc_write(struct file *file, const char __user *buf,
                                    size_t len, loff_t *ppos)
{
    struct miscdevice *miscdev = file->private_data;
    struct quard_star_ipc *ipc = container_of(miscdev, struct quard_star_ipc,
                                              miscdev);
    unsigned char payload[QUARD_STAR_IPC_MAX_MSG];
    int ret;

    if (!len)
        return 0;

    if (len > QUARD_STAR_IPC_MAX_MSG)
        return -EMSGSIZE;

    if (copy_from_user(payload, buf, len))
        return -EFAULT;

    mutex_lock(&ipc->lock);
    ret = quard_star_ipc_push(&ipc->shared->linux_to_rtos,
                              &ipc->shared->notify_pending_l2r,
                              payload, len);
    mutex_unlock(&ipc->lock);
    if (ret < 0)
        return ret;

    return ret;
}

static __poll_t quard_star_ipc_poll(struct file *file, poll_table *wait)
{
    struct miscdevice *miscdev = file->private_data;
    struct quard_star_ipc *ipc = container_of(miscdev, struct quard_star_ipc,
                                              miscdev);
    __poll_t mask = 0;
    unsigned int prod;
    unsigned int cons;

    mutex_lock(&ipc->lock);

    prod = __smp_load_acquire(&ipc->shared->rtos_to_linux.prod);
    cons = READ_ONCE(ipc->shared->rtos_to_linux.cons);
    if (prod != cons)
        mask |= EPOLLIN | EPOLLRDNORM;

    prod = READ_ONCE(ipc->shared->linux_to_rtos.prod);
    cons = __smp_load_acquire(&ipc->shared->linux_to_rtos.cons);
    if (ipc->shared->linux_to_rtos.size - (prod - cons) > sizeof(unsigned int))
        mask |= EPOLLOUT | EPOLLWRNORM;

    mutex_unlock(&ipc->lock);
    return mask;
}

static const struct file_operations quard_star_ipc_fops = {
    .owner = THIS_MODULE,
    .read = quard_star_ipc_read,
    .write = quard_star_ipc_write,
    .poll = quard_star_ipc_poll,
    .llseek = no_llseek,
};

static int quard_star_ipc_probe(struct platform_device *pdev)
{
    struct quard_star_ipc *ipc;
    struct device_node *memory;
    struct resource res;
    void *base;
    int ret;

    ipc = devm_kzalloc(&pdev->dev, sizeof(*ipc), GFP_KERNEL);
    if (!ipc)
        return -ENOMEM;

    memory = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
    if (!memory)
        return -EINVAL;

    ret = of_address_to_resource(memory, 0, &res);
    of_node_put(memory);
    if (ret)
        return ret;

    if (resource_size(&res) < sizeof(*ipc->shared))
        return -EINVAL;

    base = devm_memremap(&pdev->dev, res.start, resource_size(&res), MEMREMAP_WB);
    if (IS_ERR(base))
        return PTR_ERR(base);

    ipc->shared = base;
    ipc->phys = res.start;
    ipc->size = resource_size(&res);
    mutex_init(&ipc->lock);
    quard_star_ipc_init_shared(ipc);

    ipc->miscdev.minor = MISC_DYNAMIC_MINOR;
    ipc->miscdev.name = "quard_star_ipc";
    ipc->miscdev.fops = &quard_star_ipc_fops;
    ipc->miscdev.parent = &pdev->dev;
    ipc->miscdev.mode = 0666;

    ret = misc_register(&ipc->miscdev);
    if (ret)
        return ret;

    platform_set_drvdata(pdev, ipc);
    dev_info(&pdev->dev, "shared IPC ready at %pa (/dev/%s)\n", &ipc->phys,
             ipc->miscdev.name);
    return 0;
}

static int quard_star_ipc_remove(struct platform_device *pdev)
{
    struct quard_star_ipc *ipc = platform_get_drvdata(pdev);

    misc_deregister(&ipc->miscdev);
    return 0;
}

static const struct of_device_id quard_star_ipc_of_match[] = {
    { .compatible = "quard,quard-star-ipc" },
    { }
};
MODULE_DEVICE_TABLE(of, quard_star_ipc_of_match);

static struct platform_driver quard_star_ipc_driver = {
    .probe = quard_star_ipc_probe,
    .remove = quard_star_ipc_remove,
    .driver = {
        .name = "quard_star_ipc",
        .of_match_table = quard_star_ipc_of_match,
    },
};
module_platform_driver(quard_star_ipc_driver);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Quard Star shared-memory IPC driver");
MODULE_LICENSE("GPL");
