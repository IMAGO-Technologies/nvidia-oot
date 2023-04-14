/*
 * drivers/video/tegra/host/nvdla/nvdla.h
 *
 * Tegra Graphics Host NVDLA
 *
 * Copyright (c) 2016 NVIDIA Corporation.  All rights reserved.
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

#ifndef __NVHOST_NVDLA_H__
#define __NVHOST_NVDLA_H__

#define NV_DLA_THI_METHOD_ID	0x00000040      /* RW-4R */
#define NV_DLA_THI_METHOD_DATA	0x00000044      /* RW-4R */
#define MAX_NVDLA_QUEUE_COUNT	16

/* data structure to keep device data */
struct nvdla_device {
	struct platform_device *pdev;
	struct nvhost_queue_pool *pool;
};

extern const struct file_operations tegra_nvdla_ctrl_ops;
extern struct nvhost_queue_ops nvdla_queue_ops;

int nvhost_nvdla_finalize_poweron(struct platform_device *pdev);
int nvhost_nvdla_prepare_poweroff(struct platform_device *pdev);
int nvhost_nvdla_flcn_isr(struct platform_device *pdev);
void nvdla_send_cmd(struct platform_device *pdev,
			uint32_t method_id, uint32_t method_data);

#endif /* end of __NVHOST_NVDLA_H__ */
