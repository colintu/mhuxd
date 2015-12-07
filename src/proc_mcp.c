/*
 *  mhuxd - mircoHam device mutliplexer/demultiplexer
 *  Copyright (C) 2012-2015  Matthias Moeller, DJ5QV
 *
 *  This program can be distributed under the terms of the GNU GPLv2.
 *  See the file COPYING
 */

#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include "proc_mcp.h"
#include "util.h"
#include "buffer.h"
#include "logger.h"
#include "mhcontrol.h"
#include "mhmk2r.h"
#include "mhinfo.h"

#define MOD_ID "mcp"

#define MCP_MAX_CMD_SIZE (32)

struct proc_mcp {
	struct mh_control *ctl;
	char cmd[MCP_MAX_CMD_SIZE + 1];
	uint8_t cmd_len;
	unsigned cmd_overflow;
	const char *action_name;
	int fd;
};

struct proc_mcp *mcp_create(struct mh_control *ctl) {
	struct proc_mcp *mcp;
	dbg1("%s()", __func__);
	mcp = w_calloc(1, sizeof(*mcp));
	mcp->ctl = ctl;
	return mcp;
}

void mcp_destroy(struct proc_mcp *mcp) {
	dbg1("%s()", __func__);
	free(mcp);
}

static void send_response(int fd, const char *cmd, const char *arg) {
	ssize_t r;
	char response[MCP_MAX_CMD_SIZE + 3];
	snprintf(response, sizeof(response), "%s%s\r", cmd, arg);
	r = write(fd, response, strlen(response));
	if(r <= 0)
		err_e(errno, "%s() could not write response!", __func__);
}

static void send_err_response(int fd, const char *cmd) {
	ssize_t r;
	char response[MCP_MAX_CMD_SIZE + 4];
	*response = 'E';
	strcpy(response + 1, cmd);
	strcat(response, "\r");
	r = write(fd, response, strlen(response));
	if(r <= 0)
		err_e(errno, "%s() could not write response!", __func__);
}

static void completion_cb(unsigned const char *reply_buf, int len, int result, void *user_data)  {
	(void)reply_buf; (void)len;
	struct proc_mcp *mcp = user_data;

	if(result != CMD_RESULT_OK) {
		err("%s command failed: %s!", mcp->action_name, mhc_cmd_err_string(result));
		send_err_response(mcp->fd, mcp->cmd);
		return;
	}
	dbg1("%s cmd ok", mcp->action_name);
}

static int frd_to_hfocus(uint8_t hfocus[8], const char *frd_arg) {
	int i;

	if(strlen(frd_arg) != 12)
		return -1;

	for(i = 0; i < 12; i++)
		if(frd_arg[i] != '0' && frd_arg[i] != '1')
			return -1;

	i = 0;

	mk2r_set_hfocus_value(hfocus, "ears.left.r1Main", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.left.r1Sub", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.left.scLeft", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.left.scRight", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.left.r2Main", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.left.r2Sub", frd_arg[i++] == '1');

	mk2r_set_hfocus_value(hfocus, "ears.right.r1Main", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.right.r1Sub", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.right.scLeft", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.right.scRight", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.right.r2Main", frd_arg[i++] == '1');
	mk2r_set_hfocus_value(hfocus, "ears.right.r2Sub", frd_arg[i++] == '1');

	mk2r_set_hfocus_value(hfocus, "directControl", 1);

	return 0;
}

static int cmd_am(struct proc_mcp *mcp) {
	uint8_t acc_outputs[4];
	uint8_t offset = (mcp->cmd[2] == '2' ? 2 : 0);
	int i;
	uint16_t acc_int;

	mhc_mk2r_get_acc_outputs(mcp->ctl, acc_outputs);


	if(strlen(mcp->cmd) == 3) {
		// query
		char arg[16 + 1];
		char *p = arg;
		arg[16] = 0;
		acc_int = acc_outputs[offset] << 8 | acc_outputs[offset + 1];

		for(i = 0; i < 16; i++) {
			*p++ = ((acc_int >> (15 - i)) & 1) ? '1' : '0';
		}
		send_response(mcp->fd, mcp->cmd, arg);
		return 0;
	}

	if(strlen(mcp->cmd) == 19) {
		// set
		char *p = mcp->cmd + 3;
		acc_int = 0;

		for(i = 0; i < 16; i++) {
			switch(p[i]) {
			case '0':
				break;
			case '1':
				acc_int |= (acc_int | 1 << (15 - i));
				break;
			default:
				err("%s Invalid parameter!", mcp->cmd);
				return -1;
				break;

			}
		}

		acc_outputs[offset] = acc_int >> 8;
		acc_outputs[offset + 1] = acc_int & 0xff;

		return mhc_mk2r_set_acc_outputs(mcp->ctl, acc_outputs, completion_cb, mcp);
	}

	err("%s Invalid parameter!", mcp->cmd);
	return -1;
}

static int cmd_ver(struct proc_mcp *mcp) {
	char c = mcp->cmd[1];

	if(c == 'S' || c == 0) {
		send_response(mcp->fd, "VS", mhc_get_serial(mcp->ctl));
	}
	if(c == 'F' || c == 0) {
		const struct mh_info *mhi = mhc_get_mhinfo(mcp->ctl);
		char arg[10];
		snprintf(arg, sizeof(arg), "%02d.%02d", mhi->ver_fw_major, mhi->ver_fw_minor);
		send_response(mcp->fd, "VF", arg);
	}
	if(c == 'R' || c == 0) {
		char *arg = "08.05.06";
		send_response(mcp->fd, "VR", arg);
	}

	if(c == 0) {
		// Though not so in the specs, Windows router seems to add connection status here.
		char *arg = mhc_is_online(mcp->ctl) ? "1" : "0";
		send_response(mcp->fd, "C", arg);
	}

	return 0;
}

//static int am_to_acc(uint8_t acc[16]

/*
 * FT1<CR> SetTxFocus(R1)
 * FT2<CR> SetTxFocus(R2)
 * FR1<CR> SetRxFocus(R1)
 * FR2<CR> SetRxFocus(R2)
 * FRS<CR> SetRxFocus(STEREO)
 * FRDxxxxxxxxxxxx<CR> SetRxFocus(DIRECT)
 * AM1xxxxxxxxxxxxxxxx<CR> SetAccOutputs(R1, outputs) -> TBC
 * AM2xxxxxxxxxxxxxxxx<CR> SetAccOutputs(R2, outputs) -> TBC
 * AS1dd<CR> SetAccOutputSelection(R1, selection) -> TBC
 * AS2dd<CR> SetAccOutputSelection(R2, selection) -> TBC
 * SAs<CR> ApplyScenario(scenarioIndex) -> mhc/APPLY SCENARIO
 * MA<CR> AbortMessage() -> mhc/ABORT CW/FSK MESSAGE
 * MPm<CR> PlayMessage(msgIndex) -> mhc/PLAY CW/FSK MESSAGE
 * MPImi<CR> PlayMessagePeriodically(msgIndex, interval) -> TBC, probably to be implemented in router
 * MRm<CR> StartMessageRecording(msgIndex) -> mhc/RECORD CW/FSK MESSAGE
 * MRS<CR> StopMessageRecording() -> mhc/RECORD CW/FSK MESSAGE
 * MBname<CR> SetMessageBank(msgName) -> TBC, probably implemented in router
 *
 */
static int process_cmd(struct proc_mcp *mcp) {
	uint8_t hfocus[8];

	if(mcp->cmd_len < 1)
		return -1;

	dbg1("command: %s", mcp->cmd);

	mhc_mk2r_get_hfocus(mcp->ctl, hfocus);

	if(!strcmp(mcp->cmd, "FT1")) {
		mk2r_set_hfocus_value(hfocus, "txFocus", 0);
		goto set_hfocus;
	}

	if(!strcmp(mcp->cmd, "FT2")) {
		mk2r_set_hfocus_value(hfocus, "txFocus", 1);
		goto set_hfocus;
	}

	if(!strcmp(mcp->cmd, "FR1")) {
		mk2r_set_hfocus_value(hfocus, "rxFocus", 0);
		mk2r_set_hfocus_value(hfocus, "stereoFocus", 0);
		mk2r_set_hfocus_value(hfocus, "directControl", 0);
		goto set_hfocus;
	}

	if(!strcmp(mcp->cmd, "FR2")) {
		mk2r_set_hfocus_value(hfocus, "rxFocus", 1);
		mk2r_set_hfocus_value(hfocus, "stereoFocus", 0);
		mk2r_set_hfocus_value(hfocus, "directControl", 0);
		goto set_hfocus;
	}

	if(!strcmp(mcp->cmd, "FRS")) {
		mk2r_set_hfocus_value(hfocus, "stereoFocus", 1);
		mk2r_set_hfocus_value(hfocus, "directControl", 0);
		goto set_hfocus;
	}

	if(!strncmp(mcp->cmd, "FRD", 3)) {
		if(0 == frd_to_hfocus(hfocus, mcp->cmd + 3))
			goto set_hfocus;
	}

	if(!strncmp(mcp->cmd, "AM1", 3) || !strncmp(mcp->cmd, "AM2", 3)) {
		return cmd_am(mcp);
	}

	if(!strcmp(mcp->cmd,"VS") || !strcmp(mcp->cmd,"VF") || !strcmp(mcp->cmd,"VR") || !strcmp(mcp->cmd,"V")) {
		return cmd_ver(mcp);
	}

	if(!strcmp(mcp->cmd, "C")) {
		char *arg = mhc_is_online(mcp->ctl) ? "1" : "0";
		send_response(mcp->fd, "C", arg);
		return 0;
	}

	if(!strncmp(mcp->cmd, "SA", 2) && strlen(mcp->cmd) == 3) {
		char arg[2];
		arg[0] = mcp->cmd[2];
		arg[1] = 0;
		if(arg[0] >= '0' && arg[0] <= '7') {
			mcp->action_name = "APPLY SCENARIO";
			return mhc_mk2r_set_scenario(mcp->ctl, atoi(arg), completion_cb, mcp);
		}
	}

	if(!strncmp(mcp->cmd, "MR", 2) && strlen(mcp->cmd) == 3) {
		char arg[2];
		arg[0] = mcp->cmd[2];
		arg[1] = 0;
		if(arg[0] >= '1' && arg[0] <= '9') {
			mcp->action_name = "RECORD MESSAGE";
			return mhc_record_message(mcp->ctl, atoi(arg), completion_cb, mcp);
		}
	}

	if(!strncmp(mcp->cmd, "MP", 2) && strlen(mcp->cmd) == 3) {
		char arg[2];
		arg[0] = mcp->cmd[2];
		arg[1] = 0;
		if(arg[0] >= '1' && arg[0] <= '9') {
			mcp->action_name = "PLAY MESSAGE";
			return mhc_play_message(mcp->ctl, atoi(arg), completion_cb, mcp);
		}
	}

	if(!strcmp(mcp->cmd, "MRS")) {
		mcp->action_name = "STOP RECORDING";
		return mhc_stop_recording(mcp->ctl, completion_cb, mcp);
	}

	if(!strcmp(mcp->cmd, "MA")) {
		mcp->action_name = "ABORT MESSAGE";
		return mhc_abort_message(mcp->ctl, completion_cb, mcp);
	}



	err("invalid command: %s", mcp->cmd);

	return -1;

set_hfocus:
	mcp->action_name = "HOST FOCUS";
	return mhc_mk2r_set_hfocus(mcp->ctl, hfocus, completion_cb, mcp);

}

void mcp_cb(struct mh_router *router, int channel, struct buffer *b, int fd, void *user_data) {
	(void)router; (void)channel;
	struct proc_mcp *mcp = user_data;
	int c;

	dbg1("%s()", __func__);

	mcp->fd = fd;

	while(-1 != (c = buf_get_c(b))) {

		if(mcp->cmd_overflow) {
			if(c == 0x0d || c == 0x0a) {
				mcp->cmd_overflow = 0;
				mcp->cmd_len = 0;
				send_err_response(fd, mcp->cmd);
			}
			continue;
		}

		if(c == 0x0d || c == 0x0a) {
			if(!mcp->cmd_len)
				continue;

			mcp->cmd[mcp->cmd_len] = 0;
			if(-1 == process_cmd(mcp)) {
				err("error processing command: %s", mcp->cmd);
				send_err_response(fd, mcp->cmd);
			}
			mcp->cmd_len = 0;
			continue;
		}

		if(mcp->cmd_len >= MCP_MAX_CMD_SIZE) {
			mcp->cmd_overflow = 1;
			err("command too long: %s(...)", mcp->cmd);
			continue;
		}

		mcp->cmd[mcp->cmd_len++] = c;
	}

	buf_reset(b);
}

