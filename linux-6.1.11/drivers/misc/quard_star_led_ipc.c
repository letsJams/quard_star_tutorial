// SPDX-License-Identifier: GPL-2.0-only
/*
 * LED proxy device backed by the Quard Star IPC transport.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/quard_star_ipc_client.h>
#include <linux/uaccess.h>

#include <quard_star_ipc.h>

static int quard_star_led_ipc_xfer(unsigned char opcode,
                                   struct quard_star_ipc_led_state *state)
{
	struct quard_star_ipc_request req = {
		.service = QUARD_STAR_IPC_SERVICE_LED,
		.opcode = opcode,
		.tx_payload = state,
		.tx_len = sizeof(*state),
		.rx_payload = state,
		.rx_len = sizeof(*state),
	};

	return quard_star_ipc_request(&req);
}

static ssize_t quard_star_led_ipc_read(struct file *file, char __user *buf,
                                       size_t len, loff_t *ppos)
{
    struct quard_star_ipc_led_state state = {
        .led = 0,
        .value = 0,
    };
    unsigned int value;
    int ret;

    if (*ppos != 0)
        return 0;
    if (len < sizeof(value))
        return -EINVAL;

    ret = quard_star_led_ipc_xfer(QUARD_STAR_IPC_LED_GET, &state);
    if (ret)
        return ret;

    value = state.value;
    if (copy_to_user(buf, &value, sizeof(value)))
        return -EFAULT;

    *ppos += sizeof(value);
    return sizeof(value);
}

static ssize_t quard_star_led_ipc_write(struct file *file, const char __user *buf,
                                        size_t len, loff_t *ppos)
{
    struct quard_star_ipc_led_state state = {
        .led = 0,
        .value = 0,
    };
    unsigned int value;
    int ret;

    if (len < sizeof(value))
        return -EINVAL;

    if (copy_from_user(&value, buf, sizeof(value)))
        return -EFAULT;

    state.value = !!value;
    ret = quard_star_led_ipc_xfer(QUARD_STAR_IPC_LED_SET, &state);
    if (ret)
        return ret;

    return sizeof(value);
}

static const struct file_operations quard_star_led_ipc_fops = {
    .owner = THIS_MODULE,
    .read = quard_star_led_ipc_read,
    .write = quard_star_led_ipc_write,
    .llseek = no_llseek,
};

static struct miscdevice quard_star_led_ipc_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "quard_star_led_ipc",
    .fops = &quard_star_led_ipc_fops,
    .mode = 0666,
};

static int __init quard_star_led_ipc_init(void)
{
    return misc_register(&quard_star_led_ipc_miscdev);
}

static void __exit quard_star_led_ipc_exit(void)
{
    misc_deregister(&quard_star_led_ipc_miscdev);
}

module_init(quard_star_led_ipc_init);
module_exit(quard_star_led_ipc_exit);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Quard Star LED proxy over IPC");
MODULE_LICENSE("GPL");
