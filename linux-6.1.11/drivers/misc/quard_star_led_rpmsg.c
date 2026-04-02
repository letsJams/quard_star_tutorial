// SPDX-License-Identifier: GPL-2.0-only
/*
 * LED proxy backed by the Quard Star rpmsg transport.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <quard_star_rpmsg.h>

struct quard_star_led_rpmsg {
	struct rpmsg_device *rpdev;
	struct miscdevice miscdev;
	struct mutex lock;
	wait_queue_head_t readq;
	struct quard_star_rpmsg_led_msg resp;
	bool resp_ready;
};

static int quard_star_led_rpmsg_xfer(struct quard_star_led_rpmsg *qsled,
				     u32 cmd, u32 value, u32 *out_value)
{
	struct quard_star_rpmsg_led_msg req = {
		.cmd = cmd,
		.led = 0,
		.value = value,
		.status = 0,
	};
	long wait_ret;
	int ret;

	mutex_lock(&qsled->lock);
	qsled->resp_ready = false;

	ret = rpmsg_send(qsled->rpdev->ept, &req, sizeof(req));
	if (ret)
		goto out_unlock;

	wait_ret = wait_event_interruptible_timeout(qsled->readq,
						    qsled->resp_ready,
						    msecs_to_jiffies(1000));
	if (wait_ret < 0) {
		ret = (int)wait_ret;
		goto out_unlock;
	}
	if (!wait_ret) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}
	if (qsled->resp.cmd != cmd) {
		ret = -EPROTO;
		goto out_unlock;
	}
	if (qsled->resp.status) {
		ret = qsled->resp.status;
		goto out_unlock;
	}

	if (out_value)
		*out_value = qsled->resp.value;

	ret = 0;

out_unlock:
	mutex_unlock(&qsled->lock);
	return ret;
}

static ssize_t quard_star_led_rpmsg_read(struct file *file, char __user *buf,
					 size_t len, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct quard_star_led_rpmsg *qsled =
		container_of(miscdev, struct quard_star_led_rpmsg, miscdev);
	u32 value;
	int ret;

	if (*ppos != 0)
		return 0;
	if (len < sizeof(value))
		return -EINVAL;

	ret = quard_star_led_rpmsg_xfer(qsled, QUARD_STAR_RPMSG_LED_GET, 0,
					&value);
	if (ret)
		return ret;
	if (copy_to_user(buf, &value, sizeof(value)))
		return -EFAULT;

	*ppos += sizeof(value);
	return sizeof(value);
}

static ssize_t quard_star_led_rpmsg_write(struct file *file,
					  const char __user *buf,
					  size_t len, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct quard_star_led_rpmsg *qsled =
		container_of(miscdev, struct quard_star_led_rpmsg, miscdev);
	u32 value;
	int ret;

	if (len < sizeof(value))
		return -EINVAL;
	if (copy_from_user(&value, buf, sizeof(value)))
		return -EFAULT;

	ret = quard_star_led_rpmsg_xfer(qsled, QUARD_STAR_RPMSG_LED_SET,
					!!value, NULL);
	if (ret)
		return ret;

	return sizeof(value);
}

static const struct file_operations quard_star_led_rpmsg_fops = {
	.owner = THIS_MODULE,
	.read = quard_star_led_rpmsg_read,
	.write = quard_star_led_rpmsg_write,
	.llseek = no_llseek,
};

static int quard_star_led_rpmsg_cb(struct rpmsg_device *rpdev, void *buf,
				   int len, void *priv, u32 src)
{
	struct quard_star_led_rpmsg *qsled = priv;

	(void)rpdev;
	(void)src;

	if (!qsled || len != sizeof(qsled->resp))
		return -EINVAL;

	memcpy(&qsled->resp, buf, sizeof(qsled->resp));
	qsled->resp_ready = true;
	wake_up_interruptible(&qsled->readq);

	return 0;
}

static int quard_star_led_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct quard_star_led_rpmsg *qsled;
	int ret;

	qsled = devm_kzalloc(&rpdev->dev, sizeof(*qsled), GFP_KERNEL);
	if (!qsled)
		return -ENOMEM;

	qsled->rpdev = rpdev;
	mutex_init(&qsled->lock);
	init_waitqueue_head(&qsled->readq);

	qsled->miscdev.minor = MISC_DYNAMIC_MINOR;
	qsled->miscdev.name = "quard_star_led_rpmsg";
	qsled->miscdev.fops = &quard_star_led_rpmsg_fops;
	qsled->miscdev.parent = &rpdev->dev;
	qsled->miscdev.mode = 0666;

	ret = misc_register(&qsled->miscdev);
	if (ret)
		return ret;

	if (rpdev->ept)
		rpdev->ept->priv = qsled;

	dev_set_drvdata(&rpdev->dev, qsled);
	dev_info(&rpdev->dev, "Quard Star LED rpmsg service ready\n");

	return 0;
}

static void quard_star_led_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct quard_star_led_rpmsg *qsled = dev_get_drvdata(&rpdev->dev);

	if (qsled)
		misc_deregister(&qsled->miscdev);
}

static const struct rpmsg_device_id quard_star_led_rpmsg_id_table[] = {
	{ .name = QUARD_STAR_RPMSG_LED_SERVICE },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, quard_star_led_rpmsg_id_table);

static struct rpmsg_driver quard_star_led_rpmsg_driver = {
	.drv.name = "quard_star_led_rpmsg",
	.id_table = quard_star_led_rpmsg_id_table,
	.probe = quard_star_led_rpmsg_probe,
	.remove = quard_star_led_rpmsg_remove,
	.callback = quard_star_led_rpmsg_cb,
};

static int __init quard_star_led_rpmsg_init(void)
{
	return register_rpmsg_driver(&quard_star_led_rpmsg_driver);
}
module_init(quard_star_led_rpmsg_init);

static void __exit quard_star_led_rpmsg_exit(void)
{
	unregister_rpmsg_driver(&quard_star_led_rpmsg_driver);
}
module_exit(quard_star_led_rpmsg_exit);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Quard Star LED proxy over rpmsg");
MODULE_LICENSE("GPL");
