// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2015-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#ifndef __CDI_MGR_PRIV_H__
#define __CDI_MGR_PRIV_H__

#include <linux/cdev.h>
#include <media/cdi-mgr.h>

#define CDI_MGR_STOP_GPIO_INTR_EVENT_WAIT	(~(0u))
#define CDI_MGR_TCA9539_REGISTER_COUNT      (8)
#define CDI_MGR_TCA9539_BASE_REG_ADDR       (0x00)

enum cam_gpio_direction {
	CAM_DEVBLK_GPIO_UNSUSED = 0,
	CAM_DEVBLK_GPIO_INPUT_INTERRUPT,
	CAM_DEVBLK_GPIO_OUTPUT
};

struct cam_gpio_config {
	int index;
	enum cam_gpio_direction gpio_dir;
	struct gpio_desc *desc;
	int gpio_intr_irq;
};

struct tca9539_priv {
	struct i2c_adapter *adap;
	int bus;
	u32 addr;
	u32 reg_len;
	u32 dat_len;
	u8 init_val[12];
	u32 power_port;
	bool enable;
};

struct cdi_mgr_priv {
	struct device *pdev; /* parent device */
	struct device *dev; /* this device */
	dev_t devt;
	struct cdev cdev;
	struct class *cdi_class;
	struct i2c_adapter *adap;
	struct cdi_mgr_platform_data *pdata;
	struct list_head dev_list;
	struct mutex mutex;
	struct dentry *d_entry;
	struct work_struct ins_work;
	struct task_struct *t;
	struct kernel_siginfo sinfo;
	int sig_no; /* store signal number from user space */
	spinlock_t spinlock;
	atomic_t in_use;
	char devname[32];
	u32 pwr_state;
	atomic_t irq_in_use;
	struct pwm_device *pwm;
	wait_queue_head_t err_queue;
	bool err_irq_reported;
	u8 des_pwr_method;
	u8 des_pwr_i2c_addr;
	struct tca9539_priv tca9539;
	struct cam_gpio_config gpio_arr[MAX_CDI_GPIOS];
	uint32_t gpio_count;
	uint32_t err_irq_recvd_status_mask;
	bool stop_err_irq_wait;
	u8 cim_ver; /* 1 - P3714 A01, 2 - P3714 A02 */
	u32 cim_frsync[3]; /* FRSYNC source selection for each muxer */
	u8 pre_suspend_tca9539_regvals[CDI_MGR_TCA9539_REGISTER_COUNT];
};

int cdi_mgr_power_up(struct cdi_mgr_priv *cdi_mgr, unsigned long arg);
int cdi_mgr_power_down(struct cdi_mgr_priv *cdi_mgr, unsigned long arg);

int cdi_mgr_debugfs_init(struct cdi_mgr_priv *cdi_mgr);
int cdi_mgr_debugfs_remove(struct cdi_mgr_priv *cdi_mgr);

#endif  /* __CDI_MGR_PRIV_H__ */
