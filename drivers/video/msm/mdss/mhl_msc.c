/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/types.h>
#include <linux/mhl_8334.h>
#include <linux/vmalloc.h>
#include <linux/input.h>
#include "mhl_msc.h"

static struct mhl_tx_ctrl *mhl_ctrl;
static DEFINE_MUTEX(msc_send_workqueue_mutex);

const char *devcap_reg_name[] = {
	"DEV_STATE       ",
	"MHL_VERSION     ",
	"DEV_CAT         ",
	"ADOPTER_ID_H    ",
	"ADOPTER_ID_L    ",
	"VID_LINK_MODE   ",
	"AUD_LINK_MODE   ",
	"VIDEO_TYPE      ",
	"LOG_DEV_MAP     ",
	"BANDWIDTH       ",
	"FEATURE_FLAG    ",
	"DEVICE_ID_H     ",
	"DEVICE_ID_L     ",
	"SCRATCHPAD_SIZE ",
	"INT_STAT_SIZE   ",
	"Reserved        ",
};

static void mhl_print_devcap(u8 offset, u8 devcap)
{
	switch (offset) {
	case DEVCAP_OFFSET_DEV_CAT:
		pr_debug("DCAP: %02X %s: %02X DEV_TYPE=%X POW=%s\n",
			offset, devcap_reg_name[offset], devcap,
			devcap & 0x0F, (devcap & 0x10) ? "y" : "n");
		break;
	case DEVCAP_OFFSET_FEATURE_FLAG:
		pr_debug("DCAP: %02X %s: %02X RCP=%s RAP=%s SP=%s\n",
			offset, devcap_reg_name[offset], devcap,
			(devcap & 0x01) ? "y" : "n",
			(devcap & 0x02) ? "y" : "n",
			(devcap & 0x04) ? "y" : "n");
		break;
	default:
		pr_debug("DCAP: %02X %s: %02X\n",
			offset, devcap_reg_name[offset], devcap);
		break;
	}
}

void mhl_register_msc(struct mhl_tx_ctrl *ctrl)
{
	if (ctrl)
		mhl_ctrl = ctrl;
}

static int mhl_flag_scrpd_burst_req(struct mhl_tx_ctrl *mhl_ctrl,
		struct msc_command_struct *req)
{
	int postpone_send = 0;

	if ((req->command == MHL_SET_INT) &&
	    (req->offset == MHL_RCHANGE_INT)) {
		if (mhl_ctrl->scrpd_busy) {
			/* reduce priority */
			if (req->payload.data[0] == MHL_INT_REQ_WRT)
				postpone_send = 1;
		} else {
			if (req->payload.data[0] == MHL_INT_REQ_WRT) {
				mhl_ctrl->scrpd_busy = true;
				mhl_ctrl->wr_burst_pending = true;
			} else if (req->payload.data[0] == MHL_INT_GRT_WRT) {
					mhl_ctrl->scrpd_busy = true;
			}
		}
	}
	return postpone_send;
}



void mhl_msc_send_work(struct work_struct *work)
{
	struct mhl_tx_ctrl *mhl_ctrl =
		container_of(work, struct mhl_tx_ctrl, mhl_msc_send_work);
	struct msc_cmd_envelope *cmd_env;
	int ret, postpone_send;
	/*
	 * Remove item from the queue
	 * and schedule it
	 */
	mutex_lock(&msc_send_workqueue_mutex);
	while (!list_empty(&mhl_ctrl->list_cmd)) {
		cmd_env = list_first_entry(&mhl_ctrl->list_cmd,
					   struct msc_cmd_envelope,
					   msc_queue_envelope);
		list_del(&cmd_env->msc_queue_envelope);
		mutex_unlock(&msc_send_workqueue_mutex);

		postpone_send = mhl_flag_scrpd_burst_req(
			mhl_ctrl,
			&cmd_env->msc_cmd_msg);
		if (postpone_send) {
			if (cmd_env->msc_cmd_msg.retry-- > 0) {
				mutex_lock(&msc_send_workqueue_mutex);
				list_add_tail(
					&cmd_env->msc_queue_envelope,
					&mhl_ctrl->list_cmd);
				mutex_unlock(&msc_send_workqueue_mutex);
			} else {
				pr_err("%s: max scrpd retry out\n",
				       __func__);
			}
		} else {
			ret = mhl_send_msc_command(mhl_ctrl,
						   &cmd_env->msc_cmd_msg);
			if (ret == -EAGAIN) {
				int retry = 2;
				while (retry--) {
					ret = mhl_send_msc_command(
						mhl_ctrl,
						&cmd_env->msc_cmd_msg);
					if (ret != -EAGAIN)
						break;
				}
			}
			if (ret == -EAGAIN)
				pr_err("%s: send_msc_command retry out!\n",
				       __func__);
			vfree(cmd_env);
		}

		mutex_lock(&msc_send_workqueue_mutex);
	}
	mutex_unlock(&msc_send_workqueue_mutex);
}

int mhl_queue_msc_command(struct mhl_tx_ctrl *mhl_ctrl,
			  struct msc_command_struct *req,
			  int priority_send)
{
	struct msc_cmd_envelope *cmd_env;

	mutex_lock(&msc_send_workqueue_mutex);
	cmd_env = vmalloc(sizeof(struct msc_cmd_envelope));
	if (!cmd_env) {
		pr_err("%s: out of memory!\n", __func__);
		return -ENOMEM;
	}

	memcpy(&cmd_env->msc_cmd_msg, req,
	       sizeof(struct msc_command_struct));

	if (priority_send)
		list_add(&cmd_env->msc_queue_envelope,
			 &mhl_ctrl->list_cmd);
	else
		list_add_tail(&cmd_env->msc_queue_envelope,
			      &mhl_ctrl->list_cmd);
	mutex_unlock(&msc_send_workqueue_mutex);
	queue_work(mhl_ctrl->msc_send_workqueue, &mhl_ctrl->mhl_msc_send_work);

	return 0;
}

static int mhl_update_devcap(struct mhl_tx_ctrl *mhl_ctrl,
	int offset, u8 devcap)
{
	if (!mhl_ctrl)
		return -EFAULT;
	if (offset < 0 || offset > 15)
		return -EFAULT;
	mhl_ctrl->devcap[offset] = devcap;
	mhl_print_devcap(offset, mhl_ctrl->devcap[offset]);

	return 0;
}


int mhl_msc_command_done(struct mhl_tx_ctrl *mhl_ctrl,
			 struct msc_command_struct *req)
{
	switch (req->command) {
	case MHL_WRITE_STAT:
		if (req->offset == MHL_STATUS_REG_LINK_MODE) {
			if (req->payload.data[0]
			    & MHL_STATUS_PATH_ENABLED)
				/* Enable TMDS output */
				mhl_tmds_ctrl(mhl_ctrl, TMDS_ENABLE);
			else
				/* Disable TMDS output */
				mhl_tmds_ctrl(mhl_ctrl, TMDS_DISABLE);
		}
		break;
	case MHL_READ_DEVCAP:
		mhl_update_devcap(mhl_ctrl,
			req->offset, req->retval);
		mhl_ctrl->devcap_state |= BIT(req->offset);
		switch (req->offset) {
		case MHL_DEV_CATEGORY_OFFSET:
			if (req->retval & MHL_DEV_CATEGORY_POW_BIT)
				pr_debug("%s: devcap pow bit set\n",
					 __func__);
			else
				pr_debug("%s: devcap pow bit unset\n",
					 __func__);
			break;
		case DEVCAP_OFFSET_MHL_VERSION:
		case DEVCAP_OFFSET_INT_STAT_SIZE:
			break;
		}
		break;
	case MHL_WRITE_BURST:
		mhl_msc_send_set_int(
			mhl_ctrl,
			MHL_RCHANGE_INT,
			MHL_INT_DSCR_CHG,
			MSC_PRIORITY_SEND);
		break;
	}
	return 0;
}

int mhl_msc_send_set_int(struct mhl_tx_ctrl *mhl_ctrl,
			 u8 offset, u8 mask, u8 prior)
{
	struct msc_command_struct req;
	req.command = MHL_SET_INT;
	req.offset = offset;
	req.payload.data[0] = mask;
	return mhl_queue_msc_command(mhl_ctrl, &req, prior);
}

int mhl_msc_send_write_stat(struct mhl_tx_ctrl *mhl_ctrl,
			    u8 offset, u8 value)
{
	struct msc_command_struct req;
	req.command = MHL_WRITE_STAT;
	req.offset = offset;
	req.payload.data[0] = value;
	return mhl_queue_msc_command(mhl_ctrl, &req, MSC_NORMAL_SEND);
}

static int mhl_msc_write_burst(struct mhl_tx_ctrl *mhl_ctrl,
	u8 offset, u8 *data, u8 length)
{
	struct msc_command_struct req;
	if (!mhl_ctrl)
		return -EFAULT;

	if (!mhl_ctrl->wr_burst_pending)
		return -EFAULT;

	req.command = MHL_WRITE_BURST;
	req.offset = offset;
	req.length = length;
	req.payload.burst_data = data;
	mhl_queue_msc_command(mhl_ctrl, &req, MSC_PRIORITY_SEND);
	mhl_ctrl->wr_burst_pending = false;
	return 0;
}



int mhl_msc_send_msc_msg(struct mhl_tx_ctrl *mhl_ctrl,
			 u8 sub_cmd, u8 cmd_data)
{
	struct msc_command_struct req;
	req.command = MHL_MSC_MSG;
	req.payload.data[0] = sub_cmd;
	req.payload.data[1] = cmd_data;
	return mhl_queue_msc_command(mhl_ctrl, &req, MSC_NORMAL_SEND);
}

/*
 * Certain MSC msgs such as RCPK, RCPE and RAPK
 * should be transmitted as a high priority
 * because these msgs should be sent within
 * 1000ms of a receipt of RCP/RAP. So such msgs can
 * be added to the head of msc cmd queue.
 */
static int mhl_msc_send_prior_msc_msg(struct mhl_tx_ctrl *mhl_ctrl,
				      u8 sub_cmd, u8 cmd_data)
{
	struct msc_command_struct req;
	req.command = MHL_MSC_MSG;
	req.payload.data[0] = sub_cmd;
	req.payload.data[1] = cmd_data;
	return mhl_queue_msc_command(mhl_ctrl, &req, MSC_PRIORITY_SEND);
}


int mhl_msc_read_devcap(struct mhl_tx_ctrl *mhl_ctrl, u8 offset)
{
	struct msc_command_struct req;
	if (offset < 0 || offset > 15)
		return -EFAULT;
	req.command = MHL_READ_DEVCAP;
	req.offset = offset;
	req.payload.data[0] = 0;
	return mhl_queue_msc_command(mhl_ctrl, &req, MSC_NORMAL_SEND);
}

int mhl_msc_read_devcap_all(struct mhl_tx_ctrl *mhl_ctrl)
{
	int offset;
	int ret;

	for (offset = 0; offset < DEVCAP_SIZE; offset++) {
		ret = mhl_msc_read_devcap(mhl_ctrl, offset);
		if (ret == -EBUSY)
			pr_err("%s: queue busy!\n", __func__);
	}
	return ret;
}


static void mhl_handle_input(struct mhl_tx_ctrl *mhl_ctrl,
			     u8 key_code, u16 input_key_code)
{
	int key_press = (key_code & 0x80) == 0;

	pr_debug("%s: send key events[%x][%d]\n",
		 __func__, key_code, key_press);
	input_report_key(mhl_ctrl->input, input_key_code, key_press);
	input_sync(mhl_ctrl->input);
}



int mhl_rcp_recv(struct mhl_tx_ctrl *mhl_ctrl, u8 key_code)
{
	u8 index = key_code & 0x7f;
	u16 input_key_code;

	if (!mhl_ctrl->rcp_key_code_tbl) {
		pr_err("%s: RCP Key Code Table not initialized\n", __func__);
		return -EINVAL;
	}

	input_key_code = mhl_ctrl->rcp_key_code_tbl[index];

	if ((index < mhl_ctrl->rcp_key_code_tbl_len) &&
	    (input_key_code > 0)) {
		/* prior send rcpk */
		mhl_msc_send_prior_msc_msg(
			mhl_ctrl,
			MHL_MSC_MSG_RCPK,
			key_code);

		if (mhl_ctrl->input)
			mhl_handle_input(mhl_ctrl, key_code, input_key_code);
	} else {
		/* prior send rcpe */
		mhl_msc_send_prior_msc_msg(
			mhl_ctrl,
			MHL_MSC_MSG_RCPE,
			MHL_RCPE_INEFFECTIVE_KEY_CODE);

		/* send rcpk after rcpe send */
		mhl_msc_send_prior_msc_msg(
			mhl_ctrl,
			MHL_MSC_MSG_RCPK,
			key_code);
	}
	return 0;
}


static int mhl_rap_action(struct mhl_tx_ctrl *mhl_ctrl, u8 action_code)
{
	switch (action_code) {
	case MHL_RAP_CONTENT_ON:
		mhl_tmds_ctrl(mhl_ctrl, TMDS_ENABLE);
		break;
	case MHL_RAP_CONTENT_OFF:
		mhl_tmds_ctrl(mhl_ctrl, TMDS_DISABLE);
		break;
	default:
		break;
	}
	return 0;
}

static int mhl_rap_recv(struct mhl_tx_ctrl *mhl_ctrl, u8 action_code)
{
	u8 error_code;

	switch (action_code) {
	case MHL_RAP_POLL:
		if (mhl_ctrl->tmds_enabled())
			error_code = MHL_RAPK_NO_ERROR;
		else
			error_code = MHL_RAPK_UNSUPPORTED_ACTION_CODE;
		break;
	case MHL_RAP_CONTENT_ON:
	case MHL_RAP_CONTENT_OFF:
		mhl_rap_action(mhl_ctrl, action_code);
		error_code = MHL_RAPK_NO_ERROR;
		break;
	default:
		error_code = MHL_RAPK_UNRECOGNIZED_ACTION_CODE;
		break;
	}
	/* prior send rapk */
	return mhl_msc_send_prior_msc_msg(
		mhl_ctrl,
		MHL_MSC_MSG_RAPK,
		error_code);
}


int mhl_msc_recv_msc_msg(struct mhl_tx_ctrl *mhl_ctrl,
			 u8 sub_cmd, u8 cmd_data)
{
	int rc = 0;
	switch (sub_cmd) {
	case MHL_MSC_MSG_RCP:
		pr_debug("MHL: receive RCP(0x%02x)\n", cmd_data);
		rc = mhl_rcp_recv(mhl_ctrl, cmd_data);
		break;
	case MHL_MSC_MSG_RCPK:
		pr_debug("MHL: receive RCPK(0x%02x)\n", cmd_data);
		break;
	case MHL_MSC_MSG_RCPE:
		pr_debug("MHL: receive RCPE(0x%02x)\n", cmd_data);
		break;
	case MHL_MSC_MSG_RAP:
		pr_debug("MHL: receive RAP(0x%02x)\n", cmd_data);
		rc = mhl_rap_recv(mhl_ctrl, cmd_data);
		break;
	case MHL_MSC_MSG_RAPK:
		pr_debug("MHL: receive RAPK(0x%02x)\n", cmd_data);
		break;
	default:
		break;
	}
	return rc;
}

int mhl_msc_recv_set_int(struct mhl_tx_ctrl *mhl_ctrl,
			 u8 offset, u8 set_int)
{
	int prior;
	if (offset >= 2)
		return -EFAULT;

	switch (offset) {
	case 0:
		if (set_int & MHL_INT_DCAP_CHG) {
			/* peer dcap has changed */
			mhl_ctrl->devcap_state = 0;
			mhl_msc_read_devcap_all(mhl_ctrl);
		}
		if (set_int & MHL_INT_DSCR_CHG) {
			/* peer's scratchpad reg changed */
			pr_debug("%s: dscr chg\n", __func__);
			mhl_read_scratchpad(mhl_ctrl);
			mhl_ctrl->scrpd_busy = false;
		}
		if (set_int & MHL_INT_REQ_WRT) {
			/* SET_INT: REQ_WRT */
			if (mhl_ctrl->scrpd_busy) {
				prior = MSC_NORMAL_SEND;
			} else {
				prior = MSC_PRIORITY_SEND;
				mhl_ctrl->scrpd_busy = true;
			}
			mhl_msc_send_set_int(
				mhl_ctrl,
				MHL_RCHANGE_INT,
				MHL_INT_GRT_WRT,
				prior);
		}
		if (set_int & MHL_INT_GRT_WRT) {
			/* SET_INT: GRT_WRT */
			pr_debug("%s: recvd req to permit/grant write",
				 __func__);
			mhl_msc_write_burst(
				mhl_ctrl,
				MHL_SCRATCHPAD_OFFSET,
				mhl_ctrl->scrpd.data,
				mhl_ctrl->scrpd.length);
		}
		break;
	case 1:
		if (set_int & MHL_INT_EDID_CHG) {
			/* peer EDID has changed
			 * toggle HPD to read EDID
			 */
			pr_debug("%s: EDID CHG\n", __func__);
			mhl_drive_hpd(mhl_ctrl, HPD_DOWN);
			msleep(110);
			mhl_drive_hpd(mhl_ctrl, HPD_UP);
		}
	}
	return 0;
}

int mhl_msc_recv_write_stat(struct mhl_tx_ctrl *mhl_ctrl,
			    u8 offset, u8 value)
{
	if (offset >= 2)
		return -EFAULT;

	switch (offset) {
	case 0:
		/*
		 * connected device bits
		 * changed and DEVCAP READY
		 */
		if (((value ^ mhl_ctrl->devcap_state) &
		     MHL_STATUS_DCAP_RDY)) {
			if (value & MHL_STATUS_DCAP_RDY) {
				mhl_ctrl->devcap_state = 0;
				mhl_msc_read_devcap_all(mhl_ctrl);
			} else {
				/*
				 * peer dcap turned not ready
				 * use old devap state
				 */
				pr_debug("%s: DCAP RDY bit cleared\n",
					 __func__);
			}
		}
		break;
	case 1:
		/*
		 * connected device bits
		 * changed and PATH ENABLED
		 * bit set
		 */
		if ((value ^ mhl_ctrl->path_en_state)
		    & MHL_STATUS_PATH_ENABLED) {
			if (value & MHL_STATUS_PATH_ENABLED) {
				if (mhl_ctrl->tmds_enabled() &&
				    (mhl_ctrl->devcap[offset] &
				     MHL_FEATURE_RAP_SUPPORT)) {
					mhl_msc_send_msc_msg(
						mhl_ctrl,
						MHL_MSC_MSG_RAP,
						MHL_RAP_CONTENT_ON);
				}
				mhl_ctrl->path_en_state
					|= (MHL_STATUS_PATH_ENABLED |
					    MHL_STATUS_CLK_MODE_NORMAL);
				mhl_msc_send_write_stat(
					mhl_ctrl,
					MHL_STATUS_REG_LINK_MODE,
					mhl_ctrl->path_en_state);
			} else {
				mhl_ctrl->path_en_state
					&= ~(MHL_STATUS_PATH_ENABLED |
					     MHL_STATUS_CLK_MODE_NORMAL);
				mhl_msc_send_write_stat(
					mhl_ctrl,
					MHL_STATUS_REG_LINK_MODE,
					mhl_ctrl->path_en_state);
			}
		}
		break;
	}
	mhl_ctrl->path_en_state = value;
	return 0;
}

static int mhl_request_write_burst(struct mhl_tx_ctrl *mhl_ctrl,
				   u8 start_reg,
				   u8 length, u8 *data)
{
	int rc = 0;

	if (!(mhl_ctrl->devcap[DEVCAP_OFFSET_FEATURE_FLAG] &
	      MHL_FEATURE_SP_SUPPORT)) {
		pr_debug("MHL: SCRATCHPAD_NOT_SUPPORTED\n");
		rc = -EFAULT;
	} else {
		if (mhl_ctrl->scrpd_busy) {
			pr_debug("MHL: scratchpad_busy\n");
			rc = -EBUSY;
		} else {
			int i, reg;
			for (i = 0, reg = start_reg; (i < length) &&
				     (reg < MHL_SCRATCHPAD_SIZE); i++, reg++)
				mhl_ctrl->scrpd.data[reg] = data[i];
			mhl_ctrl->scrpd.length = length;
			mhl_ctrl->scrpd.offset = start_reg;
			mhl_msc_send_set_int(
				mhl_ctrl,
				MHL_RCHANGE_INT,
				MHL_INT_REQ_WRT,
				MSC_PRIORITY_SEND);
		}
	}
	return rc;
}

/* write scratchpad entry */
int mhl_write_scratchpad(struct mhl_tx_ctrl *mhl_ctrl,
			  u8 offset, u8 length, u8 *data)
{
	int rc;

	if ((length < ADOPTER_ID_SIZE) ||
	    (length > MAX_SCRATCHPAD_TRANSFER_SIZE) ||
	    (offset > (MAX_SCRATCHPAD_TRANSFER_SIZE - ADOPTER_ID_SIZE)) ||
	    ((offset + length) > MAX_SCRATCHPAD_TRANSFER_SIZE)) {
		pr_debug("MHL: write_burst (0x%02x)\n", -EINVAL);
		return  -EINVAL;
	}

	rc = mhl_request_write_burst(mhl_ctrl, offset, length, data);

	return rc;
}
