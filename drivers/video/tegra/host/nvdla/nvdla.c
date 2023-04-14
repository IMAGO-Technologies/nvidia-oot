/*
 * NVDLA driver for T194
 *
 * Copyright (c) 2016, NVIDIA Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>

#include "dev.h"
#include "bus_client.h"
#include "nvhost_acm.h"
#include "nvhost_buffer.h"
#include "flcn/flcn.h"
#include "flcn/hw_flcn.h"

#include "t194/t194.h"
#include "nvhost_queue.h"

#include "nvdla/nvdla.h"
#include "nvhost_nvdla_ioctl.h"
#include "dla_os_interface.h"

#define DEBUG_BUFFER_SIZE 0x100
#define FLCN_IDLE_TIMEOUT_DEFAULT	10000	/* 10 milliseconds */
#define ALIGNED_DMA(x) ((x >> 8) & 0xffffffff)

static DEFINE_DMA_ATTRS(attrs);

/* data structure to keep device data */
struct nvdla_device {
	struct platform_device *pdev;
	struct nvhost_queue_pool *pool;
};

int nvhost_nvdla_flcn_isr(struct platform_device *pdev)
{
	struct flcn *m = get_flcn(pdev);
	uint32_t mailbox0;

	/* dump falcon data if debug enabled */
	mailbox0 = host1x_readl(pdev, flcn_mailbox0_r());
	if (mailbox0 == DLA_DEBUG_PRINT)
		dev_info(&pdev->dev, "falcon: %s",
			 (char *)m->debug_dump_va);

	return 0;
}

/* Helper API's */
static void nvdla_send_cmd(struct platform_device *pdev,
		   uint32_t method_id, uint32_t method_data)
{
	host1x_writel(pdev, NV_DLA_THI_METHOD_ID, method_id);
	host1x_writel(pdev, NV_DLA_THI_METHOD_DATA, method_data);
}

static int nvdla_alloc_dump_region(struct platform_device *pdev)
{
	int err = 0;
	struct flcn *m;
	dma_addr_t region_pa;
	struct dla_region_printf *region;
	u32 timeout = FLCN_IDLE_TIMEOUT_DEFAULT * 5;
	struct nvhost_device_data *pdata = platform_get_drvdata(pdev);

	if (!pdata->flcn_isr)
		return 0;

	m = get_flcn(pdev);
	/* allocate dump region */
	m->debug_dump_va = dma_alloc_attrs(&pdev->dev,
				   DEBUG_BUFFER_SIZE, &m->debug_dump_pa,
				   GFP_KERNEL, &attrs);
	if (!m->debug_dump_va) {
		dev_err(&pdev->dev, "dma memory allocation failed");
		return -ENOMEM;
	}

	/* allocate memory for command */
	region = (struct dla_region_printf *)dma_alloc_attrs(&pdev->dev,
					sizeof(struct dla_region_printf),
					&region_pa, GFP_KERNEL, &attrs);
	if (!region) {
		dev_err(&pdev->dev, "dma memory allocation failed");
		err = -ENOMEM;
		goto set_region_failed;
	}

	region->region = DLA_REGION_PRINTF;
	region->address = ALIGNED_DMA(m->debug_dump_pa);
	region->size = DEBUG_BUFFER_SIZE;

	/* pass dump region to falcon */
	nvdla_send_cmd(pdev, DLA_CMD_SET_REGIONS,
			       ALIGNED_DMA(region_pa));

	/* wait for falcon to idle */
	err = flcn_wait_idle(pdev, &timeout);
	if (err != 0)
		dev_err(&pdev->dev, "failed for wait for idle in timeout");

	/* free memory allocated for command */
	dma_free_attrs(&pdev->dev, sizeof(struct dla_region_printf),
		       region, region_pa,
		       &attrs);

	return 0;

set_region_failed:
	dma_free_attrs(&pdev->dev, DEBUG_BUFFER_SIZE,
		       m->debug_dump_va, m->debug_dump_pa,
		       &attrs);

	return err;
}

static void nvdla_free_dump_region(struct platform_device *pdev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(pdev);
	struct flcn *m;

	if (!pdata->flcn_isr)
		return;

	m = get_flcn(pdev);
	if (m->debug_dump_pa) {
		dma_free_attrs(&pdev->dev, DEBUG_BUFFER_SIZE,
			       m->debug_dump_va, m->debug_dump_pa,
			       &attrs);
		m->debug_dump_va = NULL;
		m->debug_dump_pa = 0;

		/* reset dump region */
		nvdla_send_cmd(pdev, DLA_CMD_SET_REGIONS, m->debug_dump_pa);
	}
}

/* Queue management API */
static int nvdla_queue_abort(struct nvhost_queue *queue)
{
	/* TBD: Abort pending tasks from the queue */

	return 0;
}

static struct nvhost_queue_ops nvdla_queue_ops = {
	.abort = nvdla_queue_abort,
};

/* power management API */
int nvhost_nvdla_finalize_poweron(struct platform_device *pdev)
{
	int ret = 0;

	ret = nvhost_flcn_finalize_poweron(pdev);
	if (ret) {
		dev_err(&pdev->dev, "%s: failed to poweron\n", __func__);
		return ret;
	}

	ret = nvdla_alloc_dump_region(pdev);
	if (ret)
		nvhost_nvdla_prepare_poweroff(pdev);

	return ret;
}

int nvhost_nvdla_prepare_poweroff(struct platform_device *pdev)
{
	int ret;

	/* free dump region */
	nvdla_free_dump_region(pdev);

	ret = nvhost_flcn_prepare_poweroff(pdev);
	if (ret) {
		dev_err(&pdev->dev, "%s: failed to poweroff\n", __func__);
		return ret;
	}

	return 0;
}

/* IOCTL API's */
struct nvdla_private {
	struct platform_device *pdev;
	struct nvhost_queue *queue;
	struct nvhost_buffers *buffers;
};

static int nvdla_ctrl_pin(struct nvdla_private *priv, void *arg)
{
	u32 *handles;
	int err = 0;
	struct nvdla_ctrl_pin_unpin_args *buf_list =
			(struct nvdla_ctrl_pin_unpin_args *)arg;
	u32 count = buf_list->num_buffers;

	handles = kcalloc(count, sizeof(u32), GFP_KERNEL);
	if (!handles)
		return -ENOMEM;

	if (copy_from_user(handles, (void __user *)buf_list->buffers,
			(count * sizeof(u32)))) {
		err = -EFAULT;
		goto nvdla_buffer_cpy_err;
	}

	err = nvhost_buffer_pin(priv->buffers, handles, count);

nvdla_buffer_cpy_err:
	kfree(handles);
	return err;
}

static int nvdla_ctrl_unpin(struct nvdla_private *priv, void *arg)
{
	u32 *handles;
	int err = 0;
	struct nvdla_ctrl_pin_unpin_args *buf_list =
			(struct nvdla_ctrl_pin_unpin_args *)arg;
	u32 count = buf_list->num_buffers;

	handles = kcalloc(count, sizeof(u32), GFP_KERNEL);
	if (!handles)
		return -ENOMEM;

	if (copy_from_user(handles, (void __user *)buf_list->buffers,
		(count * sizeof(u32)))) {
		err = -EFAULT;
		goto nvdla_buffer_cpy_err;
	}

	nvhost_buffer_unpin(priv->buffers, handles, count);

nvdla_buffer_cpy_err:
	kfree(handles);
	return err;
}

static int nvdla_ctrl_ping(struct platform_device *pdev,
			   struct nvdla_ctrl_ping_args *args)
{
	DEFINE_DMA_ATTRS(ping_attrs);
	dma_addr_t ping_pa;
	u32 *ping_va;

	uint32_t mailbox0;
	uint32_t mailbox1;
	u32 timeout = FLCN_IDLE_TIMEOUT_DEFAULT * 5;
	int err = 0;

	/* make sure that device is powered on */
	nvhost_module_busy(pdev);

	/* allocate ping buffer */
	ping_va = dma_alloc_attrs(&pdev->dev,
				  DEBUG_BUFFER_SIZE, &ping_pa,
				  GFP_KERNEL, &ping_attrs);
	if (!ping_va) {
		dev_err(&pdev->dev, "dma memory allocation failed for ping");
		err = -ENOMEM;
		goto fail_to_alloc;
	}

	/* pass ping value to falcon */
	*ping_va = args->in_challenge;

	/* run ping cmd */
	nvdla_send_cmd(pdev, DLA_CMD_PING, ALIGNED_DMA(ping_pa));

	/* wait for falcon to idle */
	err = flcn_wait_idle(pdev, &timeout);
	if (err != 0) {
		dev_err(&pdev->dev, "failed for wait for idle in timeout");
		goto fail_to_idle;
	}

	/* mailbox0 should have (in_challenge * 2) */
	mailbox0 = host1x_readl(pdev, flcn_mailbox0_r());

	/* mailbox1 should have (in_challenge * 3) */
	mailbox1 = host1x_readl(pdev, flcn_mailbox1_r());

	/* out value should have (in_challenge * 4) */
	args->out_response = *ping_va;

	if ((mailbox0 != args->in_challenge*2) ||
	    (mailbox1 != args->in_challenge*3) ||
	    (args->out_response != args->in_challenge*4)) {
		dev_err(&pdev->dev, "ping cmd failed. Falcon is not active");
		err = -EINVAL;
	}

fail_to_idle:
	if (ping_va)
		dma_free_attrs(&pdev->dev, DEBUG_BUFFER_SIZE,
			       ping_va, ping_pa, &attrs);
fail_to_alloc:
	nvhost_module_idle(pdev);

	return err;
}

static long nvdla_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct nvdla_private *priv = file->private_data;
	struct platform_device *pdev = priv->pdev;
	u8 buf[NVDLA_IOCTL_CTRL_MAX_ARG_SIZE] __aligned(sizeof(u64));
	int err = 0;

	/* check for valid IOCTL cmd */
	if ((_IOC_TYPE(cmd) != NVHOST_NVDLA_IOCTL_MAGIC) ||
	    (_IOC_NR(cmd) == 0) ||
	    (_IOC_NR(cmd) > NVDLA_IOCTL_CTRL_LAST) ||
	    (_IOC_SIZE(cmd) > NVDLA_IOCTL_CTRL_MAX_ARG_SIZE)) {
		return -ENOIOCTLCMD;
	}

	nvhost_dbg_fn("%s: pdev:%p priv:%p\n", __func__, pdev, priv);

	/* copy from user for read commands */
	if (_IOC_DIR(cmd) & _IOC_WRITE)
		if (copy_from_user(buf, (void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;

	/* handle IOCTL cmd */
	switch (cmd) {
	case NVDLA_IOCTL_CTRL_PING:
		err = nvdla_ctrl_ping(pdev, (void *)buf);
		break;
	case NVDLA_IOCTL_CTRL_PIN:
		err = nvdla_ctrl_pin(priv, (void *)buf);
		break;
	case NVDLA_IOCTL_CTRL_UNPIN:
		err = nvdla_ctrl_unpin(priv, (void *)buf);
		break;
	default:
		err = -ENOIOCTLCMD;
		break;
	}

	/* copy to user for write commands */
	if ((err == 0) && (_IOC_DIR(cmd) & _IOC_READ))
		err = copy_to_user((void __user *)arg, buf, _IOC_SIZE(cmd));

	return err;
}

static int nvdla_open(struct inode *inode, struct file *file)
{
	struct nvhost_device_data *pdata = container_of(inode->i_cdev,
					struct nvhost_device_data, ctrl_cdev);
	struct platform_device *pdev = pdata->pdev;
	struct nvdla_device *nvdla_dev = pdata->private_data;
	struct nvdla_private *priv;
	int err = 0;

	priv = kmalloc(sizeof(*priv), GFP_KERNEL);
	if (unlikely(priv == NULL)) {
		err = -ENOMEM;
		goto err_alloc_priv;
	}

	file->private_data = priv;
	priv->pdev = pdev;

	nvhost_dbg_fn("%s: pdev:%p priv:%p\n", __func__, pdev, priv);

	/* add priv to client list */
	err = nvhost_module_add_client(pdev, priv);
	if (err < 0)
		goto err_add_client;

	priv->buffers = nvhost_buffer_init(pdev);
	if (IS_ERR(priv->buffers)) {
		err = PTR_ERR(priv->buffers);
		goto err_alloc_buffer;
	}

	priv->queue = nvhost_queue_alloc(nvdla_dev->pool);
	if (IS_ERR(priv->queue)) {
		err = PTR_ERR(priv->queue);
		goto err_alloc_queue;
	}

	return nonseekable_open(inode, file);

err_alloc_queue:
	nvhost_module_remove_client(pdev, priv);
err_alloc_buffer:
	kfree(priv->buffers);
err_add_client:
	kfree(priv);
err_alloc_priv:
	return err;
}

static int nvdla_release(struct inode *inode, struct file *file)
{
	struct nvdla_private *priv = file->private_data;
	struct platform_device *pdev = priv->pdev;

	nvhost_dbg_fn("%s: pdev:%p priv:%p\n", __func__, pdev, priv);

	nvhost_queue_abort(priv->queue);
	nvhost_queue_put(priv->queue);
	nvhost_buffer_put(priv->buffers);
	nvhost_module_remove_client(pdev, priv);

	kfree(priv);
	return 0;
}

const struct file_operations tegra_nvdla_ctrl_ops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.unlocked_ioctl = nvdla_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = nvdla_ioctl,
#endif
	.open = nvdla_open,
	.release = nvdla_release,
};

/* driver probe and init */
static struct of_device_id tegra_nvdla_of_match[] = {
	{
		.name = "nvdla0",
		.compatible = "nvidia,tegra194-nvdla",
		.data = (struct nvhost_device_data *)&t19_nvdla0_info },
	{
		.name = "nvdla1",
		.compatible = "nvidia,tegra194-nvdla",
		.data = (struct nvhost_device_data *)&t19_nvdla1_info },
	{ },
};

#ifdef CONFIG_PM_GENERIC_DOMAINS
static struct of_device_id tegra_nvdla_domain_match[] = {
	{.compatible = "nvidia,tegra194-dla-pd",
	.data = (struct nvhost_device_data *)&t19_nvdla0_info},
	{},
};
#endif

static int nvdla_probe(struct platform_device *pdev)
{
	int err = 0;
	struct nvhost_device_data *pdata = NULL;
	struct nvdla_device *nvdla_dev = NULL;
	struct device *dev = &pdev->dev;

	if (pdev->dev.of_node) {
		const struct of_device_id *match;

		match = of_match_device(tegra_nvdla_of_match, dev);
		if (match)
			pdata = (struct nvhost_device_data *)match->data;
	} else {
		pdata = (struct nvhost_device_data *)pdev->dev.platform_data;
	}

	WARN_ON(!pdata);
	if (!pdata) {
		dev_info(dev, "no platform data\n");
		err = -ENODATA;
		goto err_get_pdata;
	}

	nvhost_dbg_fn("%s: pdev:%p pdata:%p\n", __func__, pdev, pdata);

	nvdla_dev = devm_kzalloc(dev, sizeof(*nvdla_dev), GFP_KERNEL);
	if (!nvdla_dev) {
		err = -ENOMEM;
		goto err_alloc_nvdla;
	}

	nvdla_dev->pdev = pdev;
	pdata->pdev = pdev;
	mutex_init(&pdata->lock);
	pdata->private_data = nvdla_dev;
	platform_set_drvdata(pdev, pdata);

	err = nvhost_client_device_get_resources(pdev);
	if (err)
		goto err_get_resources;

	err = nvhost_module_init(pdev);
	if (err)
		goto err_module_init;

#ifdef CONFIG_PM_GENERIC_DOMAINS
	err = nvhost_module_add_domain(&pdata->pd, pdev);
	if (err)
		goto err_add_domain;
#endif

	err = nvhost_client_device_init(pdev);
	if (err)
		goto err_client_device_init;

	if (pdata->flcn_isr)
		flcn_intr_init(pdev);

	nvdla_dev->pool = nvhost_queue_init(pdev, &nvdla_queue_ops,
				MAX_NVDLA_QUEUE_COUNT);
	if (IS_ERR(nvdla_dev->pool)) {
		err = PTR_ERR(nvdla_dev->pool);
		goto err_queue_init;
	}

	return 0;

err_queue_init:
	nvhost_client_device_release(pdev);
err_client_device_init:
#ifdef CONFIG_PM_GENERIC_DOMAINS
err_add_domain:
#endif
	nvhost_module_deinit(pdev);
err_module_init:
err_get_resources:
	devm_kfree(dev, nvdla_dev);
err_alloc_nvdla:
err_get_pdata:

	return err;
}

static int __exit nvdla_remove(struct platform_device *pdev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(pdev);
	struct nvdla_device *nvdla_dev = pdata->private_data;

	nvhost_queue_deinit(nvdla_dev->pool);
	nvhost_client_device_release(pdev);

	return 0;
}

static struct platform_driver nvdla_driver = {
	.probe = nvdla_probe,
	.remove = __exit_p(nvdla_remove),
	.driver = {
		.owner = THIS_MODULE,
		.name = "nvdla",
#ifdef CONFIG_OF
		.of_match_table = tegra_nvdla_of_match,
#endif
#ifdef CONFIG_PM
		.pm = &nvhost_module_pm_ops,
#endif
	},
};

static int __init nvdla_init(void)
{
	int ret;

#ifdef CONFIG_PM_GENERIC_DOMAINS
	ret = nvhost_domain_init(tegra_nvdla_domain_match);
	if (ret)
		return ret;
#endif

	return platform_driver_register(&nvdla_driver);
}

static void __exit nvdla_exit(void)
{
	platform_driver_unregister(&nvdla_driver);
}

module_init(nvdla_init);
module_exit(nvdla_exit);
MODULE_AUTHOR("Shridhar Rasal <srasal@nvidia.com>");
