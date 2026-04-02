// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared-memory virtio transport for Quard Star Linux <-> cpu7 OpenAMP.
 */

#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_ring.h>

#include <quard_star_rpmsg.h>

enum {
	QUARD_STAR_DOORBELL_L2R_SET = 0x0,
	QUARD_STAR_DOORBELL_R2L_CLR = 0xc,
};

struct quard_star_rpmsg_vring {
	void *va;
	unsigned int num;
	unsigned int align;
	struct virtqueue *vq;
};

struct quard_star_rpmsg_virtio {
	struct platform_device *pdev;
	struct virtio_device vdev;
	struct quard_star_rpmsg_rsc_vdev *rsc;
	void *shm_base;
	phys_addr_t phys;
	size_t size;
	void __iomem *doorbell;
	int irq;
	u8 status;
	struct quard_star_rpmsg_vring vrings[QUARD_STAR_RPMSG_VRING_COUNT];
};

static inline struct quard_star_rpmsg_virtio *
to_quard_star_rpmsg_virtio(struct virtio_device *vdev)
{
	return container_of(vdev, struct quard_star_rpmsg_virtio, vdev);
}

static bool quard_star_rpmsg_virtio_notify(struct virtqueue *vq)
{
	struct quard_star_rpmsg_virtio *qsrv =
		to_quard_star_rpmsg_virtio(vq->vdev);

	if (qsrv->doorbell)
		writel(1U, qsrv->doorbell + QUARD_STAR_DOORBELL_L2R_SET);

	return true;
}

static irqreturn_t quard_star_rpmsg_virtio_irq(int irq, void *data)
{
	struct quard_star_rpmsg_virtio *qsrv = data;

	if (qsrv->doorbell)
		writel(1U, qsrv->doorbell + QUARD_STAR_DOORBELL_R2L_CLR);

	if (!qsrv->vrings[0].vq && !qsrv->vrings[1].vq)
		return IRQ_NONE;

	return IRQ_WAKE_THREAD;
}

static irqreturn_t quard_star_rpmsg_virtio_irq_thread(int irq, void *data)
{
	struct quard_star_rpmsg_virtio *qsrv = data;
	irqreturn_t handled = IRQ_NONE;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(qsrv->vrings); i++) {
		if (qsrv->vrings[i].vq)
			handled |= vring_interrupt(irq, qsrv->vrings[i].vq);
	}

	return handled;
}

static void quard_star_rpmsg_virtio_init_layout(struct quard_star_rpmsg_virtio *qsrv)
{
	struct quard_star_rpmsg_rsc_vdev *rsc = qsrv->rsc;

	memset(qsrv->shm_base, 0, QUARD_STAR_RPMSG_BUF_OFFSET);

	rsc->type = QUARD_STAR_RPMSG_RESOURCE_VDEV;
	rsc->id = QUARD_STAR_RPMSG_VIRTIO_ID_RPMSG;
	rsc->notifyid = QUARD_STAR_RPMSG_VDEV_NOTIFYID;
	rsc->dfeatures = BIT(QUARD_STAR_RPMSG_F_NS);
	rsc->gfeatures = 0;
	rsc->config_len = 0;
	rsc->status = 0;
	rsc->num_of_vrings = QUARD_STAR_RPMSG_VRING_COUNT;
	rsc->reserved[0] = 0;
	rsc->reserved[1] = 0;

	rsc->vring[0].da = (u32)(qsrv->phys + QUARD_STAR_RPMSG_VRING0_OFFSET);
	rsc->vring[0].align = QUARD_STAR_RPMSG_VRING_ALIGN;
	rsc->vring[0].num = QUARD_STAR_RPMSG_VRING_NUM;
	rsc->vring[0].notifyid = QUARD_STAR_RPMSG_VRING0_NOTIFYID;
	rsc->vring[0].reserved = 0;

	rsc->vring[1].da = (u32)(qsrv->phys + QUARD_STAR_RPMSG_VRING1_OFFSET);
	rsc->vring[1].align = QUARD_STAR_RPMSG_VRING_ALIGN;
	rsc->vring[1].num = QUARD_STAR_RPMSG_VRING_NUM;
	rsc->vring[1].notifyid = QUARD_STAR_RPMSG_VRING1_NOTIFYID;
	rsc->vring[1].reserved = 0;

	qsrv->vrings[0].va = (u8 *)qsrv->shm_base +
			     QUARD_STAR_RPMSG_VRING0_OFFSET;
	qsrv->vrings[0].num = QUARD_STAR_RPMSG_VRING_NUM;
	qsrv->vrings[0].align = QUARD_STAR_RPMSG_VRING_ALIGN;

	qsrv->vrings[1].va = (u8 *)qsrv->shm_base +
			     QUARD_STAR_RPMSG_VRING1_OFFSET;
	qsrv->vrings[1].num = QUARD_STAR_RPMSG_VRING_NUM;
	qsrv->vrings[1].align = QUARD_STAR_RPMSG_VRING_ALIGN;
}

static u64 quard_star_rpmsg_virtio_get_features(struct virtio_device *vdev)
{
	struct quard_star_rpmsg_virtio *qsrv = to_quard_star_rpmsg_virtio(vdev);

	return qsrv->rsc->dfeatures;
}

static int quard_star_rpmsg_virtio_finalize_features(struct virtio_device *vdev)
{
	struct quard_star_rpmsg_virtio *qsrv = to_quard_star_rpmsg_virtio(vdev);

	vring_transport_features(vdev);
	__virtio_clear_bit(vdev, VIRTIO_F_RING_PACKED);
	qsrv->rsc->gfeatures = (u32)vdev->features;

	return 0;
}

static int quard_star_rpmsg_virtio_find_vqs(struct virtio_device *vdev,
					    unsigned int nvqs,
					    struct virtqueue *vqs[],
					    vq_callback_t *callbacks[],
					    const char * const names[],
					    const bool *ctx,
					    struct irq_affinity *desc)
{
	struct quard_star_rpmsg_virtio *qsrv = to_quard_star_rpmsg_virtio(vdev);
	unsigned int i;
	int ret = 0;

	(void)desc;

	if (nvqs > ARRAY_SIZE(qsrv->vrings))
		return -EINVAL;

	for (i = 0; i < nvqs; i++) {
		struct quard_star_rpmsg_vring *vr = &qsrv->vrings[i];
		struct virtqueue *vq;
		size_t size;

		if (!names[i]) {
			vqs[i] = NULL;
			continue;
		}

		size = vring_size(vr->num, vr->align);
		memset(vr->va, 0, size);

		vq = vring_new_virtqueue(i, vr->num, vr->align, vdev, false,
					 ctx ? ctx[i] : false, vr->va,
					 quard_star_rpmsg_virtio_notify,
					 callbacks[i], names[i]);
		if (!vq) {
			ret = -ENOMEM;
			goto error;
		}

		vq->num_max = vr->num;
		vr->vq = vq;
		vqs[i] = vq;
	}

	return 0;

error:
	for (i = 0; i < ARRAY_SIZE(qsrv->vrings); i++) {
		if (qsrv->vrings[i].vq) {
			vring_del_virtqueue(qsrv->vrings[i].vq);
			qsrv->vrings[i].vq = NULL;
		}
	}

	return ret;
}

static void quard_star_rpmsg_virtio_del_vqs(struct virtio_device *vdev)
{
	struct quard_star_rpmsg_virtio *qsrv = to_quard_star_rpmsg_virtio(vdev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(qsrv->vrings); i++) {
		if (qsrv->vrings[i].vq) {
			vring_del_virtqueue(qsrv->vrings[i].vq);
			qsrv->vrings[i].vq = NULL;
		}
	}
}

static u8 quard_star_rpmsg_virtio_get_status(struct virtio_device *vdev)
{
	struct quard_star_rpmsg_virtio *qsrv = to_quard_star_rpmsg_virtio(vdev);

	return READ_ONCE(qsrv->rsc->status);
}

static void quard_star_rpmsg_virtio_set_status(struct virtio_device *vdev,
					       u8 status)
{
	struct quard_star_rpmsg_virtio *qsrv = to_quard_star_rpmsg_virtio(vdev);

	qsrv->status = status;
	WRITE_ONCE(qsrv->rsc->status, status);
}

static void quard_star_rpmsg_virtio_reset(struct virtio_device *vdev)
{
	struct quard_star_rpmsg_virtio *qsrv = to_quard_star_rpmsg_virtio(vdev);

	qsrv->status = 0;
	WRITE_ONCE(qsrv->rsc->status, 0);
}

static void quard_star_rpmsg_virtio_get(struct virtio_device *vdev,
					unsigned int offset,
					void *buf, unsigned int len)
{
	(void)vdev;
	memset(buf, 0, len);
	(void)offset;
}

static void quard_star_rpmsg_virtio_set(struct virtio_device *vdev,
					unsigned int offset,
					const void *buf, unsigned int len)
{
	(void)vdev;
	(void)offset;
	(void)buf;
	(void)len;
}

static const struct virtio_config_ops quard_star_rpmsg_virtio_config_ops = {
	.get_features = quard_star_rpmsg_virtio_get_features,
	.finalize_features = quard_star_rpmsg_virtio_finalize_features,
	.find_vqs = quard_star_rpmsg_virtio_find_vqs,
	.del_vqs = quard_star_rpmsg_virtio_del_vqs,
	.reset = quard_star_rpmsg_virtio_reset,
	.set_status = quard_star_rpmsg_virtio_set_status,
	.get_status = quard_star_rpmsg_virtio_get_status,
	.get = quard_star_rpmsg_virtio_get,
	.set = quard_star_rpmsg_virtio_set,
};

static void quard_star_rpmsg_virtio_release(struct device *dev)
{
	struct virtio_device *vdev = dev_to_virtio(dev);
	struct quard_star_rpmsg_virtio *qsrv = to_quard_star_rpmsg_virtio(vdev);

	kfree(qsrv);
}

static int quard_star_rpmsg_virtio_probe(struct platform_device *pdev)
{
	struct quard_star_rpmsg_virtio *qsrv;
	struct device_node *doorbell;
	struct device_node *memory;
	struct resource doorbell_res;
	struct resource res;
	int ret;

	qsrv = kzalloc(sizeof(*qsrv), GFP_KERNEL);
	if (!qsrv)
		return -ENOMEM;

	qsrv->pdev = pdev;

	memory = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!memory) {
		dev_err(&pdev->dev, "missing memory-region[0]\n");
		ret = -EINVAL;
		goto err_free;
	}

	ret = of_address_to_resource(memory, 0, &res);
	of_node_put(memory);
	if (ret) {
		dev_err(&pdev->dev, "failed to translate memory-region[0]: %d\n", ret);
		goto err_free;
	}
	if (resource_size(&res) < QUARD_STAR_RPMSG_BUF_OFFSET) {
		dev_err(&pdev->dev,
			"memory-region[0] too small: 0x%llx bytes, need at least 0x%x\n",
			(unsigned long long)resource_size(&res),
			QUARD_STAR_RPMSG_BUF_OFFSET);
		ret = -EINVAL;
		goto err_free;
	}

	qsrv->shm_base = devm_memremap(&pdev->dev, res.start, resource_size(&res),
					       MEMREMAP_WB);
	if (IS_ERR(qsrv->shm_base)) {
		ret = PTR_ERR(qsrv->shm_base);
		dev_err(&pdev->dev, "memremap failed: %d\n", ret);
		goto err_free;
	}

	qsrv->phys = res.start;
	qsrv->size = resource_size(&res);
	qsrv->rsc = (struct quard_star_rpmsg_rsc_vdev *)
		((u8 *)qsrv->shm_base + QUARD_STAR_RPMSG_RSC_OFFSET);
	quard_star_rpmsg_virtio_init_layout(qsrv);

	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "dma mask setup failed: %d\n", ret);
		goto err_free;
	}

	ret = of_reserved_mem_device_init_by_idx(&pdev->dev,
						 pdev->dev.of_node, 1);
	if (ret) {
		dev_err(&pdev->dev, "reserved memory init for buffer pool failed: %d\n",
			ret);
		goto err_free;
	}

	qsrv->irq = platform_get_irq_optional(pdev, 0);

	doorbell = of_parse_phandle(pdev->dev.of_node, "doorbell", 0);
	if (doorbell) {
		ret = of_address_to_resource(doorbell, 0, &doorbell_res);
		of_node_put(doorbell);
		if (ret) {
			dev_err(&pdev->dev, "doorbell resource lookup failed: %d\n", ret);
			goto err_reserved_mem;
		}

		qsrv->doorbell = devm_ioremap(&pdev->dev, doorbell_res.start,
					      resource_size(&doorbell_res));
		if (!qsrv->doorbell) {
			ret = -ENOMEM;
			dev_err(&pdev->dev, "doorbell ioremap failed\n");
			goto err_reserved_mem;
		}
	}

	if (qsrv->irq > 0) {
		ret = devm_request_threaded_irq(&pdev->dev, qsrv->irq,
						quard_star_rpmsg_virtio_irq,
						quard_star_rpmsg_virtio_irq_thread,
						IRQF_SHARED,
						dev_name(&pdev->dev), qsrv);
		if (ret) {
			dev_err(&pdev->dev, "request_irq failed: %d\n", ret);
			goto err_reserved_mem;
		}
	}

	qsrv->vdev.id.device = VIRTIO_ID_RPMSG;
	qsrv->vdev.config = &quard_star_rpmsg_virtio_config_ops;
	qsrv->vdev.dev.parent = &pdev->dev;
	qsrv->vdev.dev.release = quard_star_rpmsg_virtio_release;

	ret = register_virtio_device(&qsrv->vdev);
	if (ret) {
		dev_err(&pdev->dev, "register_virtio_device failed: %d\n", ret);
		goto err_put_vdev;
	}

	platform_set_drvdata(pdev, qsrv);
	dev_info(&pdev->dev, "Quard Star rpmsg virtio ready at %pa\n",
		 &qsrv->phys);

	return 0;

err_reserved_mem:
	of_reserved_mem_device_release(&pdev->dev);
err_free:
	kfree(qsrv);
	return ret;

err_put_vdev:
	put_device(&qsrv->vdev.dev);
	goto err_reserved_mem;
}

static int quard_star_rpmsg_virtio_remove(struct platform_device *pdev)
{
	struct quard_star_rpmsg_virtio *qsrv = platform_get_drvdata(pdev);

	unregister_virtio_device(&qsrv->vdev);
	of_reserved_mem_device_release(&pdev->dev);

	return 0;
}

static const struct of_device_id quard_star_rpmsg_virtio_of_match[] = {
	{ .compatible = "quard,quard-star-rpmsg-virtio" },
	{ }
};
MODULE_DEVICE_TABLE(of, quard_star_rpmsg_virtio_of_match);

static struct platform_driver quard_star_rpmsg_virtio_driver = {
	.probe = quard_star_rpmsg_virtio_probe,
	.remove = quard_star_rpmsg_virtio_remove,
	.driver = {
		.name = "quard_star_rpmsg_virtio",
		.of_match_table = quard_star_rpmsg_virtio_of_match,
	},
};
module_platform_driver(quard_star_rpmsg_virtio_driver);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Quard Star shared-memory virtio rpmsg transport");
MODULE_LICENSE("GPL");
