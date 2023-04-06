// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tegra TSEC Module Support
 *
 * Copyright (c) 2022-2023, NVIDIA CORPORATION.  All rights reserved.
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


#include "tsec_comms_plat.h"
#include "tsec_comms.h"
#include "tsec_comms_regs.h"
#include "tsec_comms_cmds.h"

#define TSEC_QUEUE_POLL_INTERVAL_US (50)
#define TSEC_QUEUE_POLL_COUNT       (2000)
#define TSEC_CMD_QUEUE_PORT         (0)
#define TSEC_MSG_QUEUE_PORT         (0)
#define TSEC_EMEM_PORT              (0)
#define TSEC_QUEUE_OFFSET_MAGIC     (0x01000000)
#define TSEC_EMEM_SIZE              (0x2000)
#define TSEC_MAX_MSG_SIZE           (128)

#define DO_IPC_OVER_GSC_CO  (1)

#ifdef DO_IPC_OVER_GSC_CO
#define TSEC_BOOT_POLL_TIME_US     (100000)
#define TSEC_BOOT_POLL_INTERVAL_US (50)
#define TSEC_BOOT_POLL_COUNT       (TSEC_BOOT_POLL_TIME_US / TSEC_BOOT_POLL_INTERVAL_US)
#define TSEC_BOOT_FLAG_MAGIC       (0xA5A5A5A5)
static u64 s_ipc_gscco_base;
static u64 s_ipc_gscco_size;
static u64 s_ipc_gscco_page_base;
static u64 s_ipc_gscco_page_size;
static u64 s_ipc_gscco_page_count;
static u64 s_ipc_gscco_free_page_mask;
struct TSEC_BOOT_INFO {
	u32 bootFlag;
};
#endif

/*
 * Locally cache init message so that same can be conveyed
 * to DisplayRM when it asks for it
 */
static bool s_init_msg_rcvd;
static u8 s_init_tsec_msg[TSEC_MAX_MSG_SIZE];

/*
 * Array of structs to register client callback function
 * for every sw unit/module within tsec
 */
struct callback_t {
	callback_func_t cb_func;
	void            *cb_ctx;
};
static struct callback_t s_callbacks[RM_GSP_UNIT_END];

static int validate_cmd(struct RM_FLCN_QUEUE_HDR *cmd_hdr)
{
	if (cmd_hdr == NULL)
		return -TSEC_EINVAL;

	if ((cmd_hdr->size < RM_FLCN_QUEUE_HDR_SIZE) ||
		(cmd_hdr->unitId >= RM_GSP_UNIT_END)) {
		return -TSEC_EINVAL;
	}

	return 0;
}

static int ipc_txfr(u32 offset, u8 *buff, u32 size, bool read_msg)
{
#ifdef DO_IPC_OVER_GSC_CO
	u8 *gscCo;
	u32 idx;

	if (offset < TSEC_QUEUE_OFFSET_MAGIC) {
		plat_print(LVL_ERR,
			"Invalid Offset %x less than TSEC_QUEUE_OFFSET_MAGIC\n", offset);
		return -TSEC_EINVAL;
	}

	offset -= TSEC_QUEUE_OFFSET_MAGIC;

	if (!s_ipc_gscco_base || !s_ipc_gscco_size) {
		plat_print(LVL_ERR, "Invalid IPC GSC-CO address/size\n");
		return -TSEC_EINVAL;
	}
	if (!buff || !size) {
		plat_print(LVL_ERR, "Invalid client buf/size\n");
		return -TSEC_EINVAL;
	}
	if (offset  > s_ipc_gscco_size || ((offset + size) > s_ipc_gscco_size)) {
		plat_print(LVL_ERR, "Client buf beyond IPC GSC-CO limits\n");
		return -TSEC_EINVAL;
	}

	gscCo = (u8 *)(s_ipc_gscco_base + offset);
	if (read_msg) {
		for (idx = 0; idx < size; idx++)
			buff[idx] = gscCo[idx];
	} else {
		for (idx = 0; idx < size; idx++)
			gscCo[idx] = buff[idx];
	}

	return 0;
#else
	u32    *buff32 = (u32 *)buff;
	u32     ememc_offset = tsec_ememc_r(TSEC_EMEM_PORT);
	u32     ememd_offset = tsec_ememd_r(TSEC_EMEM_PORT);
	u32     num_words, num_bytes, reg32, i;

	if (offset < TSEC_QUEUE_OFFSET_MAGIC) {
		plat_print(LVL_ERR,
			"Invalid Offset %x less than TSEC_QUEUE_OFFSET_MAGIC\n", offset);
		return -TSEC_EINVAL;
	}

	if (!buff || !size) {
		plat_print(LVL_ERR, "Invalid client buf/size\n");
		return -TSEC_EINVAL;
	}

	offset -= TSEC_QUEUE_OFFSET_MAGIC;

	if (offset  > TSEC_EMEM_SIZE || ((offset + size) > TSEC_EMEM_SIZE)) {
		plat_print(LVL_ERR, "Client buf beyond EMEM limits\n");
		return -TSEC_EINVAL;
	}

	/*
	 * Set offset within EMEM
	 * (DRF_SHIFTMASK(NV_PGSP_EMEMC_OFFS) |
	 * DRF_SHIFTMASK(NV_PGSP_EMEMC_BLK));
	 */
	reg32 = offset & 0x00007ffc;
	if (read_msg) {
		/*
		 * Enable Auto Increment on Read
		 * PSEC_EMEMC EMEMC_AINCR
		 */
		reg32 = reg32 | 0x02000000;
	} else {
		/*
		 * Enable Auto Increment on Write
		 * PSEC_EMEMC EMEMC_AINCW
		 */
		reg32 = reg32 | 0x01000000;
	}

	/* Set number of 4 byte words and remaining residual bytes to transfer*/
	num_words = size >> 2;
	num_bytes = size & 0x3;

	/* Transfer 4 byte words */
	tsec_plat_reg_write(ememc_offset, reg32);
	for (i = 0; i < num_words; i++) {
		if (read_msg)
			buff32[i] = tsec_plat_reg_read(ememd_offset);
		else
			tsec_plat_reg_write(ememd_offset, buff32[i]);
	}

	/* Transfer residual bytes if any */
	if (num_bytes > 0) {
		u32 bytes_copied = num_words << 2;
		/*
		 * Read the contents first. If we're copying to the EMEM,
		 * we've set autoincrement on write,
		 * so reading does not modify the pointer.
		 * We can, thus, do a read/modify/write without needing
		 * to worry about the pointer having moved forward.
		 * There is no special explanation needed
		 * if we're copying from the EMEM since this is the last
		 * access to HW in that case.
		 */
		reg32 = tsec_plat_reg_read(ememd_offset);
		if (read_msg) {
			for (i = 0; i < num_bytes; i++)
				buff[bytes_copied + i] = ((u8 *)&reg32)[i];
		} else {
			for (i = 0; i < num_bytes; i++)
				((u8 *)&reg32)[i] = buff[bytes_copied + i];
			tsec_plat_reg_write(ememd_offset, reg32);
		}
	}

	return 0;
#endif
}

static int ipc_write(u32 head, u8 *pSrc, u32 num_bytes)
{
	return ipc_txfr(head, pSrc, num_bytes, false);
}

static int ipc_read(u32 tail, u8 *pdst, u32 num_bytes)
{
	return ipc_txfr(tail, pdst, num_bytes, true);
}

#ifdef DO_IPC_OVER_GSC_CO
static u32 tsec_get_boot_flag(void)
{
	struct TSEC_BOOT_INFO *bootInfo = (struct TSEC_BOOT_INFO *)(s_ipc_gscco_base);

	if (!s_ipc_gscco_base || !s_ipc_gscco_size) {
		plat_print(LVL_ERR, "%s: Invalid GSC-CO address/size\n", __func__);
		return 0;
	} else {
		return bootInfo->bootFlag;
	}
}

static void tsec_reset_boot_flag(void)
{
	struct TSEC_BOOT_INFO *bootInfo = (struct TSEC_BOOT_INFO *)(s_ipc_gscco_base);

	if (!s_ipc_gscco_base || !s_ipc_gscco_size)
		plat_print(LVL_ERR, "%s: Invalid GSC-CO address/size\n", __func__);
	else
		bootInfo->bootFlag = 0;
}
#endif

static void invoke_init_cb(void *unused)
{
	callback_func_t cb_func;
	void *cb_ctx;

	tsec_plat_acquire_comms_mutex();
	cb_func = s_callbacks[RM_GSP_UNIT_INIT].cb_func;
	cb_ctx  = s_callbacks[RM_GSP_UNIT_INIT].cb_ctx;
	s_callbacks[RM_GSP_UNIT_INIT].cb_func = NULL;
	s_callbacks[RM_GSP_UNIT_INIT].cb_ctx  = NULL;
	tsec_plat_release_comms_mutex();

	if (cb_func)
		cb_func(cb_ctx, (void *)s_init_tsec_msg);
}

void tsec_comms_drain_msg(bool invoke_cb)
{
	int i;
	u32 tail = 0;
	u32 head = 0;
	u32 msgq_head_reg;
	u32 msgq_tail_reg;
	static u32 sMsgq_start;
	struct RM_FLCN_QUEUE_HDR *msg_hdr;
	struct RM_GSP_INIT_MSG_GSP_INIT *init_msg_body;
	struct RM_FLCN_QUEUE_HDR *cached_init_msg_hdr;
	struct RM_GSP_INIT_MSG_GSP_INIT *cached_init_msg_body;
	callback_func_t cb_func = NULL;
	void *cb_ctx = NULL;
	u8 tsec_msg[TSEC_MAX_MSG_SIZE];

	msgq_head_reg = tsec_msgq_head_r(TSEC_MSG_QUEUE_PORT);
	msgq_tail_reg = tsec_msgq_tail_r(TSEC_MSG_QUEUE_PORT);
	msg_hdr =  (struct RM_FLCN_QUEUE_HDR *)(tsec_msg);
	init_msg_body = (struct RM_GSP_INIT_MSG_GSP_INIT *)
		(tsec_msg + RM_FLCN_QUEUE_HDR_SIZE);
	cached_init_msg_hdr = (struct RM_FLCN_QUEUE_HDR *)(s_init_tsec_msg);
	cached_init_msg_body = (struct RM_GSP_INIT_MSG_GSP_INIT *)
		(s_init_tsec_msg + RM_FLCN_QUEUE_HDR_SIZE);

	for (i = 0; !sMsgq_start && i < TSEC_QUEUE_POLL_COUNT; i++) {
		sMsgq_start = tsec_plat_reg_read(msgq_tail_reg);
		if (!sMsgq_start)
			tsec_plat_udelay(TSEC_QUEUE_POLL_INTERVAL_US);
	}

	if (!sMsgq_start)
		plat_print(LVL_WARN, "msgq_start=0x%x\n", sMsgq_start);

	for (i = 0; i < TSEC_QUEUE_POLL_COUNT; i++) {
		tail = tsec_plat_reg_read(msgq_tail_reg);
		head = tsec_plat_reg_read(msgq_head_reg);
		if (tail != head)
			break;
		tsec_plat_udelay(TSEC_QUEUE_POLL_INTERVAL_US);
	}

	if (head == 0 || tail == 0) {
		plat_print(LVL_ERR, "Invalid MSGQ head=0x%x, tail=0x%x\n",
			head, tail);
		goto EXIT;
	}

	if (tail == head) {
		plat_print(LVL_DBG, "Empty MSGQ tail = 0x%x head = 0x%x\n", tail, head);
		goto EXIT;
	}

	while (tail != head) {
		/* read header */
		ipc_read(tail, tsec_msg, RM_FLCN_QUEUE_HDR_SIZE);
		/* copy msg body */
		if (msg_hdr->size > RM_FLCN_QUEUE_HDR_SIZE) {
			ipc_read(tail + RM_FLCN_QUEUE_HDR_SIZE,
				tsec_msg + RM_FLCN_QUEUE_HDR_SIZE,
				msg_hdr->size - RM_FLCN_QUEUE_HDR_SIZE);
		}

		if (msg_hdr->unitId == RM_GSP_UNIT_INIT) {
			plat_print(LVL_DBG, "init_msg received\n");
			if (init_msg_body->numQueues < 2) {
				plat_print(LVL_ERR, "init_msg less queues than expected %d\n",
					init_msg_body->numQueues);
				goto FAIL;
			}
#ifdef DO_IPC_OVER_GSC_CO
			/* Poll for the Tsec booted flag and also reset it */
			for (i = 0; i < TSEC_BOOT_POLL_COUNT; i++) {
				if (tsec_get_boot_flag() == TSEC_BOOT_FLAG_MAGIC)
					break;
				tsec_plat_udelay(TSEC_BOOT_POLL_INTERVAL_US);
			}
			if (i >= TSEC_BOOT_POLL_COUNT) {
				plat_print(LVL_ERR, "Tsec GSC-CO Boot Flag not set\n");
				goto FAIL;
			} else {
				tsec_reset_boot_flag();
				plat_print(LVL_DBG, "Tsec GSC-CO Boot Flag reset done\n");
			}
#endif
			/* cache the init_msg */
			memcpy(cached_init_msg_hdr, msg_hdr, RM_FLCN_QUEUE_HDR_SIZE);
			memcpy(cached_init_msg_body, init_msg_body,
				msg_hdr->size - RM_FLCN_QUEUE_HDR_SIZE);

			/* Invoke the callback and clear it */
			tsec_plat_acquire_comms_mutex();
			s_init_msg_rcvd = true;
			if (invoke_cb) {
				cb_func = s_callbacks[msg_hdr->unitId].cb_func;
				cb_ctx  = s_callbacks[msg_hdr->unitId].cb_ctx;
				s_callbacks[msg_hdr->unitId].cb_func = NULL;
				s_callbacks[msg_hdr->unitId].cb_ctx = NULL;
			}
			tsec_plat_release_comms_mutex();
			if (cb_func && invoke_cb)
				cb_func(cb_ctx, (void *)tsec_msg);
		} else if (msg_hdr->unitId < RM_GSP_UNIT_END) {
			if (msg_hdr->unitId == RM_GSP_UNIT_HDCP22WIRED) {
				plat_print(LVL_DBG, "msg received from hdcp22 unitId 0x%x\n",
					msg_hdr->unitId);
			} else if (msg_hdr->unitId == RM_GSP_UNIT_REWIND) {
				tail = sMsgq_start;
				tsec_plat_reg_write(msgq_tail_reg, tail);
				head = tsec_plat_reg_read(msgq_head_reg);
				plat_print(LVL_DBG, "MSGQ tail rewinded\n");
				continue;
			} else {
				plat_print(LVL_DBG, "msg received from unknown unitId 0x%x\n",
					msg_hdr->unitId);
			}

			/* Invoke the callback and clear it */
			if (invoke_cb) {
				tsec_plat_acquire_comms_mutex();
				cb_func = s_callbacks[msg_hdr->unitId].cb_func;
				cb_ctx  = s_callbacks[msg_hdr->unitId].cb_ctx;
				s_callbacks[msg_hdr->unitId].cb_func = NULL;
				s_callbacks[msg_hdr->unitId].cb_ctx = NULL;
				tsec_plat_release_comms_mutex();
				if (cb_func)
					cb_func(cb_ctx, (void *)tsec_msg);
			}
		} else {
			plat_print(LVL_DBG,
				"msg received from unknown unitId 0x%x >= RM_GSP_UNIT_END\n",
				msg_hdr->unitId);
		}

FAIL:
		tail += ALIGN(msg_hdr->size, 4);
		head = tsec_plat_reg_read(msgq_head_reg);
		tsec_plat_reg_write(msgq_tail_reg, tail);
	}

EXIT:
	return;
}

void tsec_comms_initialize(u64 ipc_co_va, u64 ipc_co_va_size)
{
#ifdef DO_IPC_OVER_GSC_CO
	/* Set IPC CO Info before enabling Msg Interrupts from TSEC to CCPLEX */
	s_ipc_gscco_base = ipc_co_va;
	s_ipc_gscco_size = ipc_co_va_size;

	s_ipc_gscco_page_size = (64 * 1024);

	/* First Page Reserved */
	if (s_ipc_gscco_size > s_ipc_gscco_page_size) {
		s_ipc_gscco_page_count = (s_ipc_gscco_size -
			s_ipc_gscco_page_size) / s_ipc_gscco_page_size;
	} else {
		s_ipc_gscco_page_count = 0;
	}
	s_ipc_gscco_page_base = s_ipc_gscco_page_count ?
		s_ipc_gscco_base + s_ipc_gscco_page_size : 0;
	s_ipc_gscco_free_page_mask = ~((u64)0);
#else
	(void)ipc_co_va;
	(void)ipc_co_va_size;
#endif
}

void *tsec_comms_get_gscco_page(u32 page_number, u32 *gscco_offset)
{
#ifdef DO_IPC_OVER_GSC_CO
	u8 *page_va;

	if (!s_ipc_gscco_page_base || (page_number >= s_ipc_gscco_page_count)) {
		plat_print(LVL_ERR,
			"%s: No reserved memory for Page %d\n",
			__func__, page_number);
		return NULL;
	}

	page_va = (u8 *)s_ipc_gscco_page_base;
	page_va += (page_number * s_ipc_gscco_page_size);
	if (gscco_offset) {
		*gscco_offset =
			(u32)((s_ipc_gscco_page_base - s_ipc_gscco_base) +
			(page_number * s_ipc_gscco_page_size));
	}
	return page_va;
#else
	plat_print(LVL_ERR, "%s: IPC over GSC-CO not enabled\n", __func__);
	return NULL;
#endif
}
EXPORT_SYMBOL_COMMS(tsec_comms_get_gscco_page);

void *tsec_comms_alloc_mem_from_gscco(u32 size_in_bytes, u32 *gscco_offset)
{
#ifdef DO_IPC_OVER_GSC_CO
	void *page_va;
	u32 page_number;
	u64 mask;

	/* memory allocated must fit within 1 page */
	if (size_in_bytes > s_ipc_gscco_page_size) {
		plat_print(LVL_ERR,
			"%s: size %d is larger than page size\n",
			__func__, size_in_bytes);
		return NULL;
	}
	/* there must be atleast 1 page free */
	if (s_ipc_gscco_free_page_mask == 0) {
		plat_print(LVL_ERR,
			"%s: No free page\n", __func__);
		return NULL;
	}

	/* find a free page */
	page_number = 0;
	mask = 0x1;
	while (!(s_ipc_gscco_free_page_mask & mask)) {
		mask       <<= 1;
		page_number += 1;
	}

	/* allocate page */
	page_va = tsec_comms_get_gscco_page(page_number, gscco_offset);
	if (page_va)
		s_ipc_gscco_free_page_mask &= ~(mask);

	return page_va;
#else
	plat_print(LVL_ERR, "%s: IPC over GSC-CO not enabled\n", __func__);
	return NULL;
#endif
}
EXPORT_SYMBOL_COMMS(tsec_comms_alloc_mem_from_gscco);

void tsec_comms_free_gscco_mem(void *page_va)
{
#ifdef DO_IPC_OVER_GSC_CO
	u64 page_addr = (u64)page_va;
	u64 gscco_page_start = s_ipc_gscco_page_base;
	u64 gscco_page_end = s_ipc_gscco_page_base +
		(s_ipc_gscco_page_count * s_ipc_gscco_page_size);
	u64 page_number = (page_addr - gscco_page_start) /
		s_ipc_gscco_page_size;

	if ((page_addr >= gscco_page_start) &&
	    (page_addr < gscco_page_end) &&
	    (!(page_addr % s_ipc_gscco_page_size)))
		s_ipc_gscco_free_page_mask |= ((u64)0x1 << page_number);
#endif
}
EXPORT_SYMBOL_COMMS(tsec_comms_free_gscco_mem);


int tsec_comms_send_cmd(void *cmd, u32 queue_id,
	callback_func_t cb_func, void *cb_ctx)
{
	int i;
	int placeholder;
	u32 head;
	u32 tail;
	u8  cmd_size;
	u32 cmd_size_aligned;
	u32 cmdq_head_reg;
	u32 cmdq_tail_reg;
	static u32 sCmdq_size = 0x80;
	static u32 sCmdq_start;
	struct RM_FLCN_QUEUE_HDR *cmd_hdr;
	struct RM_FLCN_QUEUE_HDR hdr;

	if (!s_init_msg_rcvd) {
		plat_print(LVL_ERR, "TSEC RISCV hasn't booted successfully\n");
		return -TSEC_ENODEV;
	}

	if (queue_id != TSEC_CMD_QUEUE_PORT)
		return -TSEC_EINVAL;

	cmdq_head_reg = tsec_cmdq_head_r(TSEC_CMD_QUEUE_PORT);
	cmdq_tail_reg = tsec_cmdq_tail_r(TSEC_CMD_QUEUE_PORT);

	for (i = 0; !sCmdq_start && i < TSEC_QUEUE_POLL_COUNT; i++) {
		sCmdq_start = tsec_plat_reg_read(cmdq_tail_reg);
		if (!sCmdq_start)
			tsec_plat_udelay(TSEC_QUEUE_POLL_INTERVAL_US);
	}

	if (!sCmdq_start) {
		plat_print(LVL_WARN, "cmdq_start=0x%x\n", sCmdq_start);
		return -TSEC_ENODEV;
	}

	if (validate_cmd(cmd)) {
		plat_print(LVL_DBG, "CMD: %s: %d Invalid command\n",
			__func__, __LINE__);
		return -TSEC_EINVAL;
	}

	cmd_hdr = (struct RM_FLCN_QUEUE_HDR *)cmd;
	tsec_plat_acquire_comms_mutex();
	if (s_callbacks[cmd_hdr->unitId].cb_func) {
		tsec_plat_release_comms_mutex();
		plat_print(LVL_ERR, "more than 1 outstanding cmd for unit 0x%x\n",
			cmd_hdr->unitId);
		return -TSEC_EINVAL;
	}
	tsec_plat_release_comms_mutex();
	cmd_size = cmd_hdr->size;
	placeholder = ALIGN(cmd_size, 4);
	if (placeholder < 0) {
		plat_print(LVL_ERR, "Alignment found to be negative\n");
		return -TSEC_EINVAL;
	}
	cmd_size_aligned = (unsigned int) placeholder;
	head = tsec_plat_reg_read(cmdq_head_reg);

check_space:
	tail = tsec_plat_reg_read(cmdq_tail_reg);
	if (head < sCmdq_start || tail < sCmdq_start)
		plat_print(LVL_ERR, "head/tail less than sCmdq_start, h=0x%x,t=0x%x\n",
			head, tail);
	if (UINT_MAX - head < cmd_size_aligned) {
		pr_err("addition of head and offset wraps\n");
		return -EINVAL;
	}
	if (tail > head) {
		if ((head + cmd_size_aligned) < tail)
			goto enqueue;
		tsec_plat_udelay(TSEC_QUEUE_POLL_INTERVAL_US);
		goto check_space;
	} else {
		if ((head + cmd_size_aligned) < (sCmdq_start + sCmdq_size)) {
			goto enqueue;
		} else {
			if ((sCmdq_start + cmd_size_aligned) < tail) {
				goto rewind;
			} else {
				tsec_plat_udelay(TSEC_QUEUE_POLL_INTERVAL_US);
				goto check_space;
			}
		}
	}

rewind:
	hdr.unitId = RM_GSP_UNIT_REWIND;
	hdr.size = RM_FLCN_QUEUE_HDR_SIZE;
	hdr.ctrlFlags = 0;
	hdr.seqNumId = 0;
	if (ipc_write(head, (u8 *)&hdr, hdr.size))
		return -TSEC_EINVAL;
	head = sCmdq_start;
	tsec_plat_reg_write(cmdq_head_reg, head);
	plat_print(LVL_DBG, "CMDQ: rewind h=%x,t=%x\n", head, tail);

enqueue:
	tsec_plat_acquire_comms_mutex();
	s_callbacks[cmd_hdr->unitId].cb_func = cb_func;
	s_callbacks[cmd_hdr->unitId].cb_ctx  = cb_ctx;
	tsec_plat_release_comms_mutex();
	if (ipc_write(head, (u8 *)cmd, cmd_size)) {
		tsec_plat_acquire_comms_mutex();
		s_callbacks[cmd_hdr->unitId].cb_func = NULL;
		s_callbacks[cmd_hdr->unitId].cb_ctx  = NULL;
		tsec_plat_release_comms_mutex();
		return -TSEC_EINVAL;
	}
	head += cmd_size_aligned;
	tsec_plat_reg_write(cmdq_head_reg, head);

	plat_print(LVL_DBG, "Cmd sent to unit 0x%x\n", cmd_hdr->unitId);

	return 0;
}
EXPORT_SYMBOL_COMMS(tsec_comms_send_cmd);

int tsec_comms_set_init_cb(callback_func_t cb_func, void *cb_ctx)
{
	int err = 0;

	tsec_plat_acquire_comms_mutex();

	if (s_callbacks[RM_GSP_UNIT_INIT].cb_func) {
		plat_print(LVL_ERR, "%s: %d: INIT unit cb_func already set\n",
			__func__, __LINE__);
		err = -TSEC_EINVAL;
		goto FAIL;
	}
	if (!cb_func) {
		plat_print(LVL_ERR, "%s: %d: Init CallBack NULL\n",
			__func__, __LINE__);
		err = -TSEC_EINVAL;
		goto FAIL;
	}

	s_callbacks[RM_GSP_UNIT_INIT].cb_func = cb_func;
	s_callbacks[RM_GSP_UNIT_INIT].cb_ctx = cb_ctx;

	if (s_init_msg_rcvd) {
		plat_print(LVL_DBG, "Init msg already received invoking callback\n");
		tsec_plat_queue_work(invoke_init_cb, NULL);
	}
#ifdef DO_IPC_OVER_GSC_CO
	else if (tsec_get_boot_flag() == TSEC_BOOT_FLAG_MAGIC) {
		plat_print(LVL_DBG, "Doorbell missed tsec booted first, invoke init callback\n");
		/* Interrupt missed as tsec booted first
		 * Explicitly call drain_msg
		 */
		tsec_plat_release_comms_mutex();
		tsec_comms_drain_msg(false);
		tsec_plat_acquire_comms_mutex();
		/* Init message is drained now, hence queue the work item to invoke init callback*/
		tsec_plat_queue_work(invoke_init_cb, NULL);
	}
#endif

FAIL:
	tsec_plat_release_comms_mutex();
	return err;
}
EXPORT_SYMBOL_COMMS(tsec_comms_set_init_cb);

void tsec_comms_clear_init_cb(void)
{
	tsec_plat_acquire_comms_mutex();
	s_callbacks[RM_GSP_UNIT_INIT].cb_func = NULL;
	s_callbacks[RM_GSP_UNIT_INIT].cb_ctx  = NULL;
	tsec_plat_release_comms_mutex();
}
EXPORT_SYMBOL_COMMS(tsec_comms_clear_init_cb);
