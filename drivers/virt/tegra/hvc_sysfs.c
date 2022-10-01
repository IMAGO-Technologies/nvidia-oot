/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <soc/tegra/fuse.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mm.h>
#include <linux/bug.h>
#include <linux/io.h>
#include <soc/tegra/virt/syscalls.h>
#include <soc/tegra/virt/hv-ivc.h>
#define TEGRA_HV_ERR(...) pr_err("hvc_sysfs: " __VA_ARGS__)
#define TEGRA_HV_INFO(...) pr_info("hvc_sysfs: " __VA_ARGS__)


/*
 * This file implements a hypervisor control driver that can be accessed
 * from user-space via the sysfs interface. Currently, the only supported
 * use case is retrieval of the HV trace log when it is available.
 */

struct hyp_shared_memory_info {
	const char *node_name;
	struct bin_attribute attr;
	uint64_t ipa;
	unsigned long size;
};

enum HYP_SHM_ID {
	HYP_SHM_ID_LOG,
	HYP_SHM_ID_PCT,
	HYP_SHM_ID_NUM
};

static struct hyp_shared_memory_info hyp_shared_memory_attrs[HYP_SHM_ID_NUM];



/* Map the HV trace buffer to the calling user process */
static int hvc_sysfs_mmap(struct file *fp, struct kobject *ko,
	struct bin_attribute *attr, struct vm_area_struct *vma)
{
	struct hyp_shared_memory_info *hyp_shm_info =
		container_of(attr, struct hyp_shared_memory_info, attr);

	if ((hyp_shm_info->ipa == 0) || (hyp_shm_info->size == 0))
		return -EINVAL;


	if ((vma->vm_end - vma->vm_start) != attr->size)
		return -EINVAL;

	return remap_pfn_range(
		vma,
		vma->vm_start,
		hyp_shm_info->ipa >> PAGE_SHIFT,
		hyp_shm_info->size,
		vma->vm_page_prot);
}

/* Discover availability and placement of the HV trace buffer */
static int hvc_create_sysfs(
	struct kobject *kobj,
	struct hyp_shared_memory_info *hyp_shm_info)
{
	sysfs_bin_attr_init((struct bin_attribute *)&hyp_shm_info->attr);

	hyp_shm_info->attr.attr.name = hyp_shm_info->node_name;
	hyp_shm_info->attr.attr.mode = 0400;
	hyp_shm_info->attr.mmap = hvc_sysfs_mmap;
	hyp_shm_info->attr.size = (size_t)hyp_shm_info->size;

	if ((hyp_shm_info->ipa == 0) || (hyp_shm_info->size == 0))
		return -EINVAL;

	return sysfs_create_bin_file(kobj, &hyp_shm_info->attr);
}

static ssize_t log_mask_read(struct file *fp, struct kobject *ko,
	struct bin_attribute *attr, char *buf, loff_t pos, size_t size)
{
	if (size == sizeof(uint64_t))
		hyp_trace_get_mask((uint64_t *)buf);
	return size;
}

static ssize_t log_mask_write(struct file *fp, struct kobject *ko,
	struct bin_attribute *attr, char *buf, loff_t pos, size_t size)
{
	if (size == sizeof(uint64_t))
		hyp_trace_set_mask(*(uint64_t *)buf);
	return size;
}

static struct bin_attribute log_mask_attr;
static int create_log_mask_node(struct kobject *kobj)
{
	if (kobj == NULL)
		return -EINVAL;

	sysfs_bin_attr_init((struct bin_attribute *)&log_mask_attr);
	log_mask_attr.attr.name = "log_mask";
	log_mask_attr.attr.mode = 0600;
	log_mask_attr.read = log_mask_read;
	log_mask_attr.write = log_mask_write;
	log_mask_attr.size = sizeof(uint64_t);
	return sysfs_create_bin_file(kobj, &log_mask_attr);
}

/* Set up all relevant hypervisor control nodes */
static int __init hvc_sysfs_register(void)
{
	struct kobject *kobj;
	int ret;
	uint64_t ipa;
	struct hyp_info_page *info;
	uint64_t log_mask;

	if (is_tegra_hypervisor_mode() == false) {
		TEGRA_HV_INFO("hypervisor is not present\n");
		/*retunring success in case of native kernel otherwise
		  systemd-modules-load service will failed.*/
		return 0;
	}

	kobj = kobject_create_and_add("hvc", NULL);
	if (kobj == NULL) {
		TEGRA_HV_INFO("failed to add kobject\n");
		return -ENOMEM;
	}

	if (hyp_read_hyp_info(&ipa) != 0)
		return -EINVAL;

	info = (__force struct hyp_info_page *)ioremap(ipa, sizeof(*info));
	if (info == NULL)
		return -EFAULT;

	if (info->log_size != 0) {
		hyp_shared_memory_attrs[HYP_SHM_ID_LOG].ipa = info->log_ipa;
		hyp_shared_memory_attrs[HYP_SHM_ID_LOG].size =
			(size_t)info->log_size;
		hyp_shared_memory_attrs[HYP_SHM_ID_LOG].node_name = "log";

		ret = hvc_create_sysfs(kobj,
			&hyp_shared_memory_attrs[HYP_SHM_ID_LOG]);
		if (ret == 0)
			TEGRA_HV_INFO("log is available\n");
		else
			TEGRA_HV_INFO("log is unavailable\n");

		if (ret == 0 && hyp_trace_get_mask(&log_mask) == 0) {
			ret = create_log_mask_node(kobj);
			if (ret == 0)
				TEGRA_HV_INFO(
					"access to trace event mask is available\n");
		}
	}

	hyp_shared_memory_attrs[HYP_SHM_ID_PCT].ipa = info->pct_ipa;
	hyp_shared_memory_attrs[HYP_SHM_ID_PCT].size = (size_t)info->pct_size;
	hyp_shared_memory_attrs[HYP_SHM_ID_PCT].node_name = "pct";

	ret = hvc_create_sysfs(kobj, &hyp_shared_memory_attrs[HYP_SHM_ID_PCT]);
	if (ret == 0)
		TEGRA_HV_INFO("pct is available\n");
	else
		TEGRA_HV_INFO("pct is unavailable\n");

	iounmap((void __iomem *)info);

	return 0;
}

late_initcall(hvc_sysfs_register);

MODULE_LICENSE("GPL");
