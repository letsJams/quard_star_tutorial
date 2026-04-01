// SPDX-License-Identifier: GPL-2.0-only
/*
 * Demochar proxy device backed by the Quard Star IPC transport.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/quard_star_ipc_client.h>
#include <linux/uaccess.h>

#include <quard_star_ipc.h>

static int quard_star_demochar_ipc_xfer(unsigned char opcode,
                                        const void *payload,
                                        unsigned int payload_len,
                                        void *reply_payload,
                                        unsigned int reply_payload_len)
{
	struct quard_star_ipc_request req = {
		.service = QUARD_STAR_IPC_SERVICE_DEMOCHAR,
		.opcode = opcode,
		.tx_payload = payload,
		.tx_len = payload_len,
		.rx_payload = reply_payload,
		.rx_len = reply_payload_len,
	};

	return quard_star_ipc_request(&req);
}

static ssize_t quard_star_demochar_ipc_read(struct file *file, char __user *buf,
                                            size_t len, loff_t *ppos)
{
    struct quard_star_ipc_demochar_value value;
    int ret;

    if (*ppos != 0)
        return 0;
    if (len < sizeof(value.value))
        return -EINVAL;

    ret = quard_star_demochar_ipc_xfer(QUARD_STAR_IPC_DEMOCHAR_READ,
                                       NULL, 0, &value, sizeof(value));
    if (ret)
        return ret;

    if (copy_to_user(buf, &value.value, sizeof(value.value)))
        return -EFAULT;

    *ppos += sizeof(value.value);
    return sizeof(value.value);
}

static ssize_t quard_star_demochar_ipc_write(struct file *file,
                                             const char __user *buf,
                                             size_t len, loff_t *ppos)
{
    struct quard_star_ipc_demochar_value value;
    int ret;

    if (len < sizeof(value.value))
        return -EINVAL;

    if (copy_from_user(&value.value, buf, sizeof(value.value)))
        return -EFAULT;

    ret = quard_star_demochar_ipc_xfer(QUARD_STAR_IPC_DEMOCHAR_WRITE,
                                       &value, sizeof(value), NULL, 0);
    if (ret)
        return ret;

    return sizeof(value.value);
}

static const struct file_operations quard_star_demochar_ipc_fops = {
    .owner = THIS_MODULE,
    .read = quard_star_demochar_ipc_read,
    .write = quard_star_demochar_ipc_write,
    .llseek = no_llseek,
};

static struct miscdevice quard_star_demochar_ipc_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "quard_star_demochar_ipc",
    .fops = &quard_star_demochar_ipc_fops,
    .mode = 0666,
};

static int __init quard_star_demochar_ipc_init(void)
{
    return misc_register(&quard_star_demochar_ipc_miscdev);
}

static void __exit quard_star_demochar_ipc_exit(void)
{
    misc_deregister(&quard_star_demochar_ipc_miscdev);
}

module_init(quard_star_demochar_ipc_init);
module_exit(quard_star_demochar_ipc_exit);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Quard Star demochar proxy over IPC");
MODULE_LICENSE("GPL");
