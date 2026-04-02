#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <FreeRTOS.h>
#include <task.h>
#include <metal/io.h>
#include <metal/sys.h>
#include <openamp/open_amp.h>
#include "debug_log.h"
#include "doorbell.h"
#include "quard_star_openamp.h"
#include "quard_star.h"
#include "quard_star_rpmsg.h"

enum {
	QUARD_STAR_GPIO_OUTPUT_EN_REG = 0x8,
	QUARD_STAR_GPIO_OUTPUT_VAL_REG = 0xc,
	QUARD_STAR_LED0 = 0,
};

static TaskHandle_t quard_star_openamp_task_handle;
static struct metal_io_region quard_star_openamp_shm_io;
static struct virtio_device *quard_star_openamp_vdev;
static struct rpmsg_virtio_device quard_star_openamp_rvdev;
static struct rpmsg_endpoint quard_star_raw_ept;
static struct rpmsg_endpoint quard_star_led_ept;
static metal_phys_addr_t quard_star_openamp_phys[] = {
	QUARD_STAR_RPMSG_SHM_BASE,
};

static inline void quard_star_writel(uint32_t value, uintptr_t addr)
{
	*(volatile uint32_t *)addr = value;
}

static inline uint32_t quard_star_readl(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static void quard_star_led_write(unsigned int led, unsigned int value)
{
	uint32_t bit = 1U << led;
	uint32_t out_en = quard_star_readl(GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_EN_REG);
	uint32_t out_val = quard_star_readl(GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_VAL_REG);

	out_en |= bit;
	if (value)
		out_val |= bit;
	else
		out_val &= ~bit;

	quard_star_writel(out_en, GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_EN_REG);
	quard_star_writel(out_val, GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_VAL_REG);
}

static unsigned int quard_star_led_read(unsigned int led)
{
	uint32_t out_val = quard_star_readl(GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_VAL_REG);

	return !!(out_val & (1U << led));
}

static void quard_star_openamp_init_layout(void)
{
	void *shm_base = (void *)(uintptr_t)QUARD_STAR_RPMSG_SHM_BASE;
	volatile struct quard_star_rpmsg_rsc_vdev *rsc =
		(volatile struct quard_star_rpmsg_rsc_vdev *)QUARD_STAR_RPMSG_SHM_BASE;

	memset(shm_base, 0, QUARD_STAR_RPMSG_SHM_SIZE);

	rsc->type = QUARD_STAR_RPMSG_RESOURCE_VDEV;
	rsc->id = QUARD_STAR_RPMSG_VIRTIO_ID_RPMSG;
	rsc->notifyid = QUARD_STAR_RPMSG_VDEV_NOTIFYID;
	rsc->dfeatures = (1U << QUARD_STAR_RPMSG_F_NS);
	rsc->gfeatures = 0;
	rsc->config_len = 0;
	rsc->status = 0;
	rsc->num_of_vrings = QUARD_STAR_RPMSG_VRING_COUNT;
	rsc->reserved[0] = 0;
	rsc->reserved[1] = 0;

	rsc->vring[0].da = QUARD_STAR_RPMSG_SHM_BASE +
			   QUARD_STAR_RPMSG_VRING0_OFFSET;
	rsc->vring[0].align = QUARD_STAR_RPMSG_VRING_ALIGN;
	rsc->vring[0].num = QUARD_STAR_RPMSG_VRING_NUM;
	rsc->vring[0].notifyid = QUARD_STAR_RPMSG_VRING0_NOTIFYID;
	rsc->vring[0].reserved = 0;

	rsc->vring[1].da = QUARD_STAR_RPMSG_SHM_BASE +
			   QUARD_STAR_RPMSG_VRING1_OFFSET;
	rsc->vring[1].align = QUARD_STAR_RPMSG_VRING_ALIGN;
	rsc->vring[1].num = QUARD_STAR_RPMSG_VRING_NUM;
	rsc->vring[1].notifyid = QUARD_STAR_RPMSG_VRING1_NOTIFYID;
	rsc->vring[1].reserved = 0;
}

static int quard_star_openamp_notify(void *priv, uint32_t id)
{
	(void)priv;
	(void)id;

	quard_star_doorbell_ring_r2l();
	return 0;
}

static int quard_star_raw_cb(struct rpmsg_endpoint *ept, void *data,
			     size_t len, uint32_t src, void *priv)
{
	char rx[128];
	char tx[128];
	int tx_len;

	(void)src;
	(void)priv;

	if (!len)
		return 0;
	if (len >= sizeof(rx))
		len = sizeof(rx) - 1;

	memcpy(rx, data, len);
	rx[len] = '\0';

	if (!strncmp(rx, "ping-", 5))
		tx_len = snprintf(tx, sizeof(tx), "pong-%s", rx + 5);
	else
		tx_len = snprintf(tx, sizeof(tx), "ack:%s", rx);

	if (tx_len < 0)
		return 0;
	if ((size_t)tx_len > sizeof(tx))
		tx_len = sizeof(tx);

	return rpmsg_send(ept, tx, tx_len);
}

static int quard_star_led_cb(struct rpmsg_endpoint *ept, void *data,
			     size_t len, uint32_t src, void *priv)
{
	struct quard_star_rpmsg_led_msg msg;

	(void)src;
	(void)priv;

	if (len != sizeof(msg))
		return -EINVAL;

	memcpy(&msg, data, sizeof(msg));
	if (msg.led != QUARD_STAR_LED0) {
		msg.status = -EINVAL;
		return rpmsg_send(ept, &msg, sizeof(msg));
	}

	switch (msg.cmd) {
	case QUARD_STAR_RPMSG_LED_GET:
		msg.value = quard_star_led_read(msg.led);
		msg.status = 0;
		break;
	case QUARD_STAR_RPMSG_LED_SET:
		quard_star_led_write(msg.led, !!msg.value);
		msg.value = quard_star_led_read(msg.led);
		msg.status = 0;
		break;
	default:
		msg.status = -ENOSYS;
		break;
	}

	return rpmsg_send(ept, &msg, sizeof(msg));
}

static int quard_star_openamp_init(void)
{
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	struct rpmsg_device *rdev;
	int ret;

	quard_star_openamp_init_layout();

	ret = metal_init(&metal_params);
	if (ret)
		return ret;

	metal_io_init(&quard_star_openamp_shm_io,
		      (void *)QUARD_STAR_RPMSG_SHM_BASE,
		      quard_star_openamp_phys,
		      QUARD_STAR_RPMSG_SHM_SIZE,
		      (unsigned int)-1,
		      0,
		      NULL);

	quard_star_openamp_vdev =
		rproc_virtio_create_vdev(RPMSG_REMOTE,
					 QUARD_STAR_RPMSG_VDEV_NOTIFYID,
					 (void *)QUARD_STAR_RPMSG_SHM_BASE,
					 &quard_star_openamp_shm_io,
					 NULL,
					 quard_star_openamp_notify,
					 NULL);
	if (!quard_star_openamp_vdev)
		return -ENOMEM;

	ret = rproc_virtio_init_vring(quard_star_openamp_vdev, 0,
				      QUARD_STAR_RPMSG_VRING0_NOTIFYID,
				      (void *)(QUARD_STAR_RPMSG_SHM_BASE +
					       QUARD_STAR_RPMSG_VRING0_OFFSET),
				      &quard_star_openamp_shm_io,
				      QUARD_STAR_RPMSG_VRING_NUM,
				      QUARD_STAR_RPMSG_VRING_ALIGN);
	if (ret)
		return ret;

	ret = rproc_virtio_init_vring(quard_star_openamp_vdev, 1,
				      QUARD_STAR_RPMSG_VRING1_NOTIFYID,
				      (void *)(QUARD_STAR_RPMSG_SHM_BASE +
					       QUARD_STAR_RPMSG_VRING1_OFFSET),
				      &quard_star_openamp_shm_io,
				      QUARD_STAR_RPMSG_VRING_NUM,
				      QUARD_STAR_RPMSG_VRING_ALIGN);
	if (ret)
		return ret;

	ret = rpmsg_init_vdev(&quard_star_openamp_rvdev,
			      quard_star_openamp_vdev,
			      NULL,
			      &quard_star_openamp_shm_io,
			      NULL);
	if (ret)
		return ret;

	rdev = &quard_star_openamp_rvdev.rdev;

	ret = rpmsg_create_ept(&quard_star_raw_ept, rdev,
			       QUARD_STAR_RPMSG_RAW_SERVICE,
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       quard_star_raw_cb, NULL);
	if (ret)
		return ret;

	ret = rpmsg_create_ept(&quard_star_led_ept, rdev,
			       QUARD_STAR_RPMSG_LED_SERVICE,
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       quard_star_led_cb, NULL);
	if (ret)
		return ret;

	debug_log("openamp ready, shm @ 0x%lx\n",
		  (unsigned long)QUARD_STAR_RPMSG_SHM_BASE);
	return 0;
}

void quard_star_openamp_handle_kick(void)
{
	if (!quard_star_openamp_vdev)
		return;

	rproc_virtio_notified(quard_star_openamp_vdev, RSC_NOTIFY_ID_ANY);
}

static void quard_star_openamp_task(void *arg)
{
	int ret;

	(void)arg;

	ret = quard_star_openamp_init();
	if (ret) {
		debug_log("openamp init failed: %d\n", ret);
		vTaskDelete(NULL);
		return;
	}

	quard_star_openamp_handle_kick();

	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		quard_star_openamp_handle_kick();
	}
}

BaseType_t quard_star_openamp_start(TaskHandle_t *task_handle)
{
	BaseType_t ret;

	ret = xTaskCreate(quard_star_openamp_task, "openamp_task", 4096,
			  NULL, 4, &quard_star_openamp_task_handle);
	if (ret == pdPASS) {
		quard_star_doorbell_register_task(quard_star_openamp_task_handle);
		if (task_handle)
			*task_handle = quard_star_openamp_task_handle;
	}

	return ret;
}
