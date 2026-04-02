// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared-memory IPC between Linux and the cpu7 FreeRTOS trusted domain.
 * V1 keeps the data path minimal: two SPSC rings and polling from both sides.
 */

#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/io.h>
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/quard_star_ipc_client.h>

#include <quard_star_ipc.h>

struct quard_star_ipc {
	struct miscdevice miscdev;
	struct mutex lock;
	struct mutex call_lock;
	wait_queue_head_t readq;
	struct quard_star_ipc_shared *shared;
	void __iomem *doorbell;
	int irq;
	phys_addr_t phys;
	size_t size;
};

static struct quard_star_ipc *quard_star_ipc_global;
static atomic_t quard_star_ipc_seq = ATOMIC_INIT(1);

enum {
    QUARD_STAR_DOORBELL_L2R_SET = 0x0,
    QUARD_STAR_DOORBELL_R2L_CLR = 0xc,
};

static bool quard_star_ipc_r2l_ready(struct quard_star_ipc *ipc)
{
    return __smp_load_acquire(&ipc->shared->rtos_to_linux.prod) !=
           READ_ONCE(ipc->shared->rtos_to_linux.cons);
}

static void quard_star_ipc_ring_doorbell_l2r(struct quard_star_ipc *ipc)
{
    if (!ipc->doorbell)
        return;

    writel(1U, ipc->doorbell + QUARD_STAR_DOORBELL_L2R_SET);
}

static void quard_star_ipc_clear_doorbell_r2l(struct quard_star_ipc *ipc)
{
    if (!ipc->doorbell)
        return;

    writel(1U, ipc->doorbell + QUARD_STAR_DOORBELL_R2L_CLR);
}

static irqreturn_t quard_star_ipc_irq(int irq, void *data)
{
    struct quard_star_ipc *ipc = data;

    quard_star_ipc_clear_doorbell_r2l(ipc);
    wake_up_interruptible(&ipc->readq);

	return IRQ_HANDLED;
}

static void quard_star_ipc_warn_bad_packet(struct quard_star_ipc *ipc,
					   const char *reason)
{
	if (ipc->miscdev.this_device)
		dev_warn_ratelimited(ipc->miscdev.this_device,
				     "bad IPC response packet: %s\n", reason);
}

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

    for (;;) {
        mutex_lock(&ipc->lock);
        ret = quard_star_ipc_pop(&ipc->shared->rtos_to_linux,
                                 &ipc->shared->notify_pending_r2l,
                                 payload, len > QUARD_STAR_IPC_MAX_MSG ?
                                 QUARD_STAR_IPC_MAX_MSG : len);
        mutex_unlock(&ipc->lock);

        if (ret != -EAGAIN)
            break;

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        ret = wait_event_interruptible(ipc->readq,
                                       quard_star_ipc_r2l_ready(ipc));
        if (ret)
            return ret;
    }

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
    if (ret >= 0)
        quard_star_ipc_ring_doorbell_l2r(ipc);
    mutex_unlock(&ipc->lock);
    if (ret < 0)
        return ret;

    return ret;
}

int quard_star_ipc_call(const void *tx, size_t tx_len, void *rx, size_t *rx_len)
{
	return quard_star_ipc_call_timeout(tx, tx_len, rx, rx_len,
					   QUARD_STAR_IPC_DEFAULT_TIMEOUT_MS);
}
EXPORT_SYMBOL_GPL(quard_star_ipc_call);

int quard_star_ipc_call_timeout(const void *tx, size_t tx_len,
				void *rx, size_t *rx_len,
				unsigned int timeout_ms)
{
	struct quard_star_ipc *ipc = READ_ONCE(quard_star_ipc_global);
	unsigned char payload[QUARD_STAR_IPC_MAX_MSG];
	unsigned long remaining;
	long wait_ret;
	size_t cap;
	int ret;

	if (!ipc)
		return -ENODEV;
	if (!tx || !rx || !rx_len || !tx_len)
		return -EINVAL;
	if (tx_len > QUARD_STAR_IPC_MAX_MSG)
		return -EMSGSIZE;

	cap = *rx_len;
	if (!cap || cap > QUARD_STAR_IPC_MAX_MSG)
		cap = QUARD_STAR_IPC_MAX_MSG;

	remaining = msecs_to_jiffies(timeout_ms ? timeout_ms :
				     QUARD_STAR_IPC_DEFAULT_TIMEOUT_MS);
	if (!remaining)
		remaining = 1;

	ret = mutex_lock_interruptible(&ipc->call_lock);
	if (ret)
		return ret;

	mutex_lock(&ipc->lock);
	ret = quard_star_ipc_push(&ipc->shared->linux_to_rtos,
				  &ipc->shared->notify_pending_l2r,
				  tx, tx_len);
	if (ret >= 0)
		quard_star_ipc_ring_doorbell_l2r(ipc);
	mutex_unlock(&ipc->lock);
	if (ret < 0)
		goto out_unlock_call;

	for (;;) {
		mutex_lock(&ipc->lock);
		ret = quard_star_ipc_pop(&ipc->shared->rtos_to_linux,
					 &ipc->shared->notify_pending_r2l,
					 payload, sizeof(payload));
		mutex_unlock(&ipc->lock);

		if (ret != -EAGAIN)
			break;

		wait_ret = wait_event_interruptible_timeout(ipc->readq,
						quard_star_ipc_r2l_ready(ipc),
						remaining);
		if (wait_ret < 0) {
			ret = (int)wait_ret;
			goto out_unlock_call;
		}
		if (!wait_ret) {
			ret = -ETIMEDOUT;
			goto out_unlock_call;
		}
		remaining = (unsigned long)wait_ret;
	}

	if (ret < 0)
		goto out_unlock_call;
	if (ret > cap) {
		ret = -EMSGSIZE;
		goto out_unlock_call;
	}

	memcpy(rx, payload, ret);
	*rx_len = ret;
	ret = 0;

out_unlock_call:
	mutex_unlock(&ipc->call_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(quard_star_ipc_call_timeout);

int quard_star_ipc_request(struct quard_star_ipc_request *req)
{
	struct quard_star_ipc_msg_hdr tx_hdr;
	struct quard_star_ipc_msg_hdr rx_hdr;
	unsigned char tx[QUARD_STAR_IPC_MAX_MSG];
	unsigned char rx[QUARD_STAR_IPC_MAX_MSG];
	struct quard_star_ipc *ipc = READ_ONCE(quard_star_ipc_global);
	size_t tx_len;
	size_t rx_len = sizeof(rx);
	unsigned int rx_payload_len;
	int ret;

	if (!req)
		return -EINVAL;
	if (!ipc)
		return -ENODEV;
	if (!req->tx_payload && req->tx_len)
		return -EINVAL;
	if (!req->rx_payload && req->rx_len)
		return -EINVAL;

	tx_len = sizeof(tx_hdr) + req->tx_len;
	if (tx_len > sizeof(tx))
		return -EMSGSIZE;

	tx_hdr.magic = QUARD_STAR_IPC_MSG_MAGIC;
	tx_hdr.version = QUARD_STAR_IPC_MSG_VERSION;
	tx_hdr.type = QUARD_STAR_IPC_MSG_TYPE_REQ;
	tx_hdr.service = req->service;
	tx_hdr.opcode = req->opcode;
	tx_hdr.seq = (unsigned int)atomic_inc_return(&quard_star_ipc_seq);
	tx_hdr.status = QUARD_STAR_IPC_STATUS_OK;
	tx_hdr.len = req->tx_len;

	memcpy(tx, &tx_hdr, sizeof(tx_hdr));
	if (req->tx_len)
		memcpy(tx + sizeof(tx_hdr), req->tx_payload, req->tx_len);

	ret = quard_star_ipc_call_timeout(tx, tx_len, rx, &rx_len,
					  req->timeout_ms);
	if (ret)
		return ret;

	if (rx_len < sizeof(rx_hdr)) {
		quard_star_ipc_warn_bad_packet(ipc, "short header");
		return -EIO;
	}

	memcpy(&rx_hdr, rx, sizeof(rx_hdr));
	if (rx_hdr.magic != QUARD_STAR_IPC_MSG_MAGIC) {
		quard_star_ipc_warn_bad_packet(ipc, "bad magic");
		return -EPROTO;
	}
	if (rx_hdr.version != QUARD_STAR_IPC_MSG_VERSION) {
		quard_star_ipc_warn_bad_packet(ipc, "bad version");
		return -EPROTO;
	}
	if (rx_hdr.type != QUARD_STAR_IPC_MSG_TYPE_RESP) {
		quard_star_ipc_warn_bad_packet(ipc, "unexpected type");
		return -EPROTO;
	}
	if (rx_hdr.service != tx_hdr.service) {
		quard_star_ipc_warn_bad_packet(ipc, "unexpected service");
		return -EPROTO;
	}
	if (rx_hdr.opcode != tx_hdr.opcode) {
		quard_star_ipc_warn_bad_packet(ipc, "unexpected opcode");
		return -EPROTO;
	}
	if (rx_hdr.seq != tx_hdr.seq) {
		quard_star_ipc_warn_bad_packet(ipc, "seq mismatch");
		return -EPROTO;
	}
	if (rx_len != sizeof(rx_hdr) + rx_hdr.len) {
		quard_star_ipc_warn_bad_packet(ipc, "length mismatch");
		return -EPROTO;
	}

	rx_payload_len = rx_hdr.len;
	if (rx_hdr.status != QUARD_STAR_IPC_STATUS_OK)
		return rx_hdr.status;
	if (rx_payload_len > req->rx_len)
		return -EMSGSIZE;
	if (!req->rx_len_out && rx_payload_len != req->rx_len)
		return -EPROTO;

	if (rx_payload_len)
		memcpy(req->rx_payload, rx + sizeof(rx_hdr), rx_payload_len);
	if (req->rx_len_out)
		*req->rx_len_out = rx_payload_len;

	return 0;
}
EXPORT_SYMBOL_GPL(quard_star_ipc_request);

static __poll_t quard_star_ipc_poll(struct file *file, poll_table *wait)
{
    struct miscdevice *miscdev = file->private_data;
    struct quard_star_ipc *ipc = container_of(miscdev, struct quard_star_ipc,
                                              miscdev);
    __poll_t mask = 0;
    unsigned int prod;
    unsigned int cons;

    poll_wait(file, &ipc->readq, wait);
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
    struct device_node *doorbell;
    struct resource doorbell_res;
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
    ipc->irq = platform_get_irq_optional(pdev, 0);

    doorbell = of_parse_phandle(pdev->dev.of_node, "doorbell", 0);
    if (doorbell) {
        ret = of_address_to_resource(doorbell, 0, &doorbell_res);
        of_node_put(doorbell);
        if (ret)
            return ret;

        ipc->doorbell = devm_ioremap(&pdev->dev, doorbell_res.start,
                                     resource_size(&doorbell_res));
        if (!ipc->doorbell)
            return -ENOMEM;
    }
    mutex_init(&ipc->lock);
    mutex_init(&ipc->call_lock);
    init_waitqueue_head(&ipc->readq);
    quard_star_ipc_init_shared(ipc);

    ipc->miscdev.minor = MISC_DYNAMIC_MINOR;
    ipc->miscdev.name = "quard_star_ipc";
    ipc->miscdev.fops = &quard_star_ipc_fops;
    ipc->miscdev.parent = &pdev->dev;
    ipc->miscdev.mode = 0666;

    ret = misc_register(&ipc->miscdev);
    if (ret)
        return ret;

    if (ipc->irq > 0) {
        ret = devm_request_irq(&pdev->dev, ipc->irq, quard_star_ipc_irq,
                               IRQF_SHARED, dev_name(&pdev->dev), ipc);
        if (ret) {
            misc_deregister(&ipc->miscdev);
            return ret;
        }
    }

    platform_set_drvdata(pdev, ipc);
    WRITE_ONCE(quard_star_ipc_global, ipc);
    dev_info(&pdev->dev, "shared IPC ready at %pa (/dev/%s)\n", &ipc->phys,
             ipc->miscdev.name);
    return 0;
}

static int quard_star_ipc_remove(struct platform_device *pdev)
{
    struct quard_star_ipc *ipc = platform_get_drvdata(pdev);

    if (READ_ONCE(quard_star_ipc_global) == ipc)
        WRITE_ONCE(quard_star_ipc_global, NULL);
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
