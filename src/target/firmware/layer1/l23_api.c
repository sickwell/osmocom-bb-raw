/* Synchronous part of GSM Layer 1: API to Layer2+ */

/* (C) 2010 by Holger Hans Peter Freyther <zecke@selfish.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#define DEBUG

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <debug.h>
#include <byteorder.h>

#include <osmocore/msgb.h>
#include <osmocore/protocol/gsm_04_08.h>
#include <comm/sercomm.h>

#include <layer1/sync.h>
#include <layer1/async.h>
#include <layer1/mframe_sched.h>
#include <layer1/prim.h>
#include <layer1/tpu_window.h>

#include <abb/twl3025.h>
#include <rf/trf6151.h>

#include <l1ctl_proto.h>

/* the size we will allocate struct msgb* for HDLC */
#define L3_MSG_HEAD 4
#define L3_MSG_SIZE (sizeof(struct l1ctl_hdr)+sizeof(struct l1ctl_info_dl)+sizeof(struct l1ctl_data_ind) + L3_MSG_HEAD)

void l1_queue_for_l2(struct msgb *msg)
{
	/* forward via serial for now */
	sercomm_sendmsg(SC_DLCI_L1A_L23, msg);
}

static enum mframe_task chan_nr2mf_task(uint8_t chan_nr)
{
	uint8_t cbits = chan_nr >> 3;
	uint8_t tn = chan_nr & 0x7;
	uint8_t lch_idx;

	if (cbits == 0x01) {
		lch_idx = 0;
		return (tn & 1) ? MF_TASK_TCH_F_ODD : MF_TASK_TCH_F_EVEN;
	} else if ((cbits & 0x1e) == 0x02) {
		lch_idx = cbits & 0x1;
		return MF_TASK_TCH_H_0 + lch_idx;
	} else if ((cbits & 0x1c) == 0x04) {
		lch_idx = cbits & 0x3;
		return MF_TASK_SDCCH4_0 + lch_idx;
	} else if ((cbits & 0x18) == 0x08) {
		lch_idx = cbits & 0x7;
		return MF_TASK_SDCCH8_0 + lch_idx;
#if 0
	} else if (cbits == 0x10) {
		/* FIXME: when to do extended BCCH? */
		return MF_TASK_BCCH_NORM;
	} else if (cbits == 0x11 || cbits == 0x12) {
		/* FIXME: how to decide CCCH norm/extd? */
		return MF_TASK_BCCH_CCCH;
#endif
	}
	return 0;
}

static int  chan_nr2dchan_type(uint8_t chan_nr)
{
	uint8_t cbits = chan_nr >> 3;

	if (cbits == 0x01) {
		return GSM_DCHAN_TCH_F;
	} else if ((cbits & 0x1e) == 0x02) {
		return GSM_DCHAN_TCH_H;
	} else if ((cbits & 0x1c) == 0x04) {
		return GSM_DCHAN_SDCCH_4;
	} else if ((cbits & 0x18) == 0x08) {
		return GSM_DCHAN_SDCCH_8;
	}
	return GSM_DCHAN_UNKNOWN;
}

static int chan_nr_is_tch(uint8_t chan_nr)
{
	return ((chan_nr >> 3) == 0x01 ||		/* TCH/F */
		((chan_nr >> 3) & 0x1e) == 0x02);	/* TCH/H */
}

static void audio_set_enabled(int enabled)
{
	twl3025_unit_enable(TWL3025_UNIT_VUL, enabled);
	twl3025_unit_enable(TWL3025_UNIT_VDL, enabled);
}

struct msgb *l1ctl_msgb_alloc(uint8_t msg_type)
{
	struct msgb *msg;
	struct l1ctl_hdr *l1h;

	msg = msgb_alloc_headroom(L3_MSG_SIZE, L3_MSG_HEAD, "l1ctl");
	if (!msg) {
		while (1) {
			puts("OOPS. Out of buffers...\n");
		}

		return NULL;
	}
	l1h = (struct l1ctl_hdr *) msgb_put(msg, sizeof(*l1h));
	l1h->msg_type = msg_type;
	l1h->flags = 0;

	msg->l1h = (uint8_t *)l1h;

	return msg;
}

struct msgb *l1_create_l2_msg(int msg_type, uint32_t fn, uint16_t snr,
			      uint16_t arfcn)
{
	struct l1ctl_info_dl *dl;
	struct msgb *msg = l1ctl_msgb_alloc(msg_type);

	dl = (struct l1ctl_info_dl *) msgb_put(msg, sizeof(*dl));
	dl->frame_nr = htonl(fn);
	dl->snr = snr;
	dl->band_arfcn = htons(arfcn);

	return msg;
}

/* receive a L1CTL_FBSB_REQ from L23 */
static void l1ctl_rx_fbsb_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_fbsb_req *sync_req = (struct l1ctl_fbsb_req *) l1h->data;

	if (sizeof(*sync_req) > msg->len) {
		printf("Short sync msg. %u\n", msg->len);
		return;
	}

	printd("L1CTL_FBSB_REQ (arfcn=%u, flags=0x%x)\n",
		ntohs(sync_req->band_arfcn), sync_req->flags);

	/* reset scheduler and hardware */
	l1s_reset();

	/* pre-set the CCCH mode */
	l1s.serving_cell.ccch_mode = sync_req->ccch_mode;

	printd("Starting FCCH Recognition\n");
	l1s_fbsb_req(1, sync_req);
}

/* receive a L1CTL_DM_EST_REQ from L23 */
static void l1ctl_rx_dm_est_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_info_ul *ul = (struct l1ctl_info_ul *) l1h->data;
	struct l1ctl_dm_est_req *est_req = (struct l1ctl_dm_est_req *) ul->payload;

	printd("L1CTL_DM_EST_REQ (arfcn=%u, chan_nr=0x%02x, tsc=%u)\n",
		ntohs(est_req->h0.band_arfcn), ul->chan_nr, est_req->tsc);

	/* configure dedicated channel state */
	l1s.dedicated.type = chan_nr2dchan_type(ul->chan_nr);
	l1s.dedicated.tsc  = est_req->tsc;
	l1s.dedicated.tn   = ul->chan_nr & 0x7;
	l1s.dedicated.h    = est_req->h;

	if (est_req->h) {
		int i;
		l1s.dedicated.h1.hsn  = est_req->h1.hsn;
		l1s.dedicated.h1.maio = est_req->h1.maio;
		l1s.dedicated.h1.n    = est_req->h1.n;
		for (i=0; i<est_req->h1.n; i++)
			l1s.dedicated.h1.ma[i] = ntohs(est_req->h1.ma[i]);
	} else {
		l1s.dedicated.h0.arfcn = ntohs(est_req->h0.band_arfcn);
	}

	/* TCH config */
	if (chan_nr_is_tch(ul->chan_nr)) {
		/* Mode */
		l1a_tch_mode_set(est_req->tch_mode);

		/* Sync */
		l1s.tch_sync = 1;	/* can be set without locking */

		/* Audio path */
		audio_set_enabled(est_req->tch_mode != GSM48_CMODE_SIGN);
	}

	/* figure out which MF tasks to enable */
	l1a_mftask_set(1 << chan_nr2mf_task(ul->chan_nr));
}

/* receive a L1CTL_DM_FREQ_REQ from L23 */
static void l1ctl_rx_dm_freq_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_info_ul *ul = (struct l1ctl_info_ul *) l1h->data;
	struct l1ctl_dm_freq_req *freq_req =
			(struct l1ctl_dm_freq_req *) ul->payload;

	printd("L1CTL_DM_FREQ_REQ (arfcn=%u, tsc=%u)\n",
		ntohs(freq_req->h0.band_arfcn), freq_req->tsc);

	/* configure dedicated channel state */
	l1s.dedicated.st_tsc  = freq_req->tsc;
	l1s.dedicated.st_h    = freq_req->h;

	if (freq_req->h) {
		int i;
		l1s.dedicated.st_h1.hsn  = freq_req->h1.hsn;
		l1s.dedicated.st_h1.maio = freq_req->h1.maio;
		l1s.dedicated.st_h1.n    = freq_req->h1.n;
		for (i=0; i<freq_req->h1.n; i++)
			l1s.dedicated.st_h1.ma[i] = ntohs(freq_req->h1.ma[i]);
	} else {
		l1s.dedicated.st_h0.arfcn = ntohs(freq_req->h0.band_arfcn);
	}

	l1a_freq_req(ntohs(freq_req->fn));
}

/* receive a L1CTL_CRYPTO_REQ from L23 */
static void l1ctl_rx_crypto_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_info_ul *ul = (struct l1ctl_info_ul *) l1h->data;
	struct l1ctl_crypto_req *cr = (struct l1ctl_crypto_req *) ul->payload;
	uint8_t key_len = msg->len - sizeof(*l1h) - sizeof(*ul) - sizeof(*cr);

	printd("L1CTL_CRYPTO_REQ (algo=A5/%u, len=%u)\n", cr->algo, key_len);

	if (cr->algo && key_len != 8) {
		printd("L1CTL_CRYPTO_REQ -> Invalid key\n");
		return;
	}

	dsp_load_ciph_param(cr->algo, cr->key);
}

/* receive a L1CTL_DM_REL_REQ from L23 */
static void l1ctl_rx_dm_rel_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_info_ul *ul = (struct l1ctl_info_ul *) l1h->data;

	printd("L1CTL_DM_REL_REQ\n");
	l1a_mftask_set(0);
	l1s.dedicated.type = GSM_DCHAN_NONE;
	l1a_txq_msgb_flush(&l1s.tx_queue[L1S_CHAN_MAIN]);
	l1a_txq_msgb_flush(&l1s.tx_queue[L1S_CHAN_SACCH]);
	l1a_meas_msgb_set(NULL);
	dsp_load_ciph_param(0, NULL);
	l1a_tch_mode_set(GSM48_CMODE_SIGN);
	audio_set_enabled(0);
}

/* receive a L1CTL_PARAM_REQ from L23 */
static void l1ctl_rx_param_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_info_ul *ul = (struct l1ctl_info_ul *) l1h->data;
	struct l1ctl_par_req *par_req = (struct l1ctl_par_req *) ul->payload;

	printd("L1CTL_PARAM_REQ (ta=%d, tx_power=%d)\n", par_req->ta,
		par_req->tx_power);

	l1s.ta = par_req->ta;
	l1s.tx_power = par_req->tx_power;
}

/* receive a L1CTL_RACH_REQ from L23 */
static void l1ctl_rx_rach_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_info_ul *ul = (struct l1ctl_info_ul *) l1h->data;
	struct l1ctl_rach_req *rach_req = (struct l1ctl_rach_req *) ul->payload;

	printd("L1CTL_RACH_REQ (ra=0x%02x, offset=%d combined=%d)\n",
		rach_req->ra, ntohs(rach_req->offset), rach_req->combined);

	l1a_rach_req(ntohs(rach_req->offset), rach_req->combined,
		rach_req->ra);
}

/* receive a L1CTL_DATA_REQ from L23 */
static void l1ctl_rx_data_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_info_ul *ul = (struct l1ctl_info_ul *) l1h->data;
	struct l1ctl_data_ind *data_ind = (struct l1ctl_data_ind *) ul->payload;
	struct llist_head *tx_queue;

	printd("L1CTL_DATA_REQ (link_id=0x%02x)\n", ul->link_id);

	msg->l3h = data_ind->data;
	if (ul->link_id & 0x40) {
		struct gsm48_hdr *gh = (struct gsm48_hdr *)(data_ind->data + 5);
		if (gh->proto_discr == GSM48_PDISC_RR
		 && gh->msg_type == GSM48_MT_RR_MEAS_REP) {
			printd("updating measurement report\n");
			l1a_meas_msgb_set(msg);
			return;
		}
		tx_queue = &l1s.tx_queue[L1S_CHAN_SACCH];
	} else
		tx_queue = &l1s.tx_queue[L1S_CHAN_MAIN];

	printd("ul=%p, ul->payload=%p, data_ind=%p, data_ind->data=%p l3h=%p\n",
		ul, ul->payload, data_ind, data_ind->data, msg->l3h);

	l1a_txq_msgb_enq(tx_queue, msg);
}

/* receive a L1CTL_PM_REQ from L23 */
static void l1ctl_rx_pm_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_pm_req *pm_req = (struct l1ctl_pm_req *) l1h->data;

	switch (pm_req->type) {
	case 1:
		l1s.pm.mode = 1;
		l1s.pm.range.arfcn_start =
				ntohs(pm_req->range.band_arfcn_from);
		l1s.pm.range.arfcn_next =
				ntohs(pm_req->range.band_arfcn_from);
		l1s.pm.range.arfcn_end =
				ntohs(pm_req->range.band_arfcn_to);
		printf("L1CTL_PM_REQ start=%u end=%u\n",
			l1s.pm.range.arfcn_start, l1s.pm.range.arfcn_end);
		break;
	}

	l1s_pm_test(1, l1s.pm.range.arfcn_next);
}

/* Transmit a L1CTL_RESET_IND or L1CTL_RESET_CONF */
void l1ctl_tx_reset(uint8_t msg_type, uint8_t reset_type)
{
	struct msgb *msg = l1ctl_msgb_alloc(msg_type);
	struct l1ctl_reset *reset_resp;
	reset_resp = (struct l1ctl_reset *)
				msgb_put(msg, sizeof(*reset_resp));
	reset_resp->type = reset_type;

	l1_queue_for_l2(msg);
}

/* receive a L1CTL_RESET_REQ from L23 */
static void l1ctl_rx_reset_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_reset *reset_req =
				(struct l1ctl_reset *) l1h->data;

	switch (reset_req->type) {
	case L1CTL_RES_T_FULL:
		printf("L1CTL_RESET_REQ: FULL!\n");
		l1s_reset();
		l1s_reset_hw();
		audio_set_enabled(0);
		l1ctl_tx_reset(L1CTL_RESET_CONF, reset_req->type);
		break;
	case L1CTL_RES_T_SCHED:
		printf("L1CTL_RESET_REQ: SCHED!\n");
		l1ctl_tx_reset(L1CTL_RESET_CONF, reset_req->type);
		sched_gsmtime_reset();
		break;
	default:
		printf("unknown L1CTL_RESET_REQ type\n");
		break;
	}
}

/* Transmit a L1CTL_CCCH_MODE_CONF */
static void l1ctl_tx_ccch_mode_conf(uint8_t ccch_mode)
{
	struct msgb *msg = l1ctl_msgb_alloc(L1CTL_CCCH_MODE_CONF);
	struct l1ctl_ccch_mode_conf *mode_conf;
	mode_conf = (struct l1ctl_ccch_mode_conf *)
				msgb_put(msg, sizeof(*mode_conf));
	mode_conf->ccch_mode = ccch_mode;

	l1_queue_for_l2(msg);
}

/* receive a L1CTL_CCCH_MODE_REQ from L23 */
static void l1ctl_rx_ccch_mode_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_ccch_mode_req *ccch_mode_req =
		(struct l1ctl_ccch_mode_req *) l1h->data;
	uint8_t ccch_mode = ccch_mode_req->ccch_mode;

	/* pre-set the CCCH mode */
	l1s.serving_cell.ccch_mode = ccch_mode;

	/* Update task */
	mframe_disable(MF_TASK_CCCH_COMB);
	mframe_disable(MF_TASK_CCCH);

	if (ccch_mode == CCCH_MODE_COMBINED)
		mframe_enable(MF_TASK_CCCH_COMB);
	else if (ccch_mode == CCCH_MODE_NON_COMBINED)
		mframe_enable(MF_TASK_CCCH);

	l1ctl_tx_ccch_mode_conf(ccch_mode);
}

/* Transmit a L1CTL_TCH_MODE_CONF */
static void l1ctl_tx_tch_mode_conf(uint8_t tch_mode)
{
	struct msgb *msg = l1ctl_msgb_alloc(L1CTL_TCH_MODE_CONF);
	struct l1ctl_tch_mode_conf *mode_conf;
	mode_conf = (struct l1ctl_tch_mode_conf *)
				msgb_put(msg, sizeof(*mode_conf));
	mode_conf->tch_mode = tch_mode;

	l1_queue_for_l2(msg);
}

/* receive a L1CTL_TCH_MODE_REQ from L23 */
static void l1ctl_rx_tch_mode_req(struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;
	struct l1ctl_tch_mode_req *tch_mode_req =
		(struct l1ctl_tch_mode_req *) l1h->data;
	uint8_t tch_mode = tch_mode_req->tch_mode;

	printd("L1CTL_TCH_MODE_REQ (mode=0x%02x)\n", tch_mode);
	tch_mode = l1a_tch_mode_set(tch_mode);

	audio_set_enabled(tch_mode != GSM48_CMODE_SIGN);

	l1s.tch_sync = 1; /* Needed for audio to work */

	l1ctl_tx_tch_mode_conf(tch_mode);
}

/* callback from SERCOMM when L2 sends a message to L1 */
static void l1a_l23_rx_cb(uint8_t dlci, struct msgb *msg)
{
	struct l1ctl_hdr *l1h = (struct l1ctl_hdr *) msg->data;

#if 0
	{
		int i;
		printf("l1a_l23_rx_cb (%u): ", msg->len);
		for (i = 0; i < msg->len; i++)
			printf("%02x ", msg->data[i]);
		puts("\n");
	}
#endif

	msg->l1h = msg->data;

	if (sizeof(*l1h) > msg->len) {
		printf("l1a_l23_cb: Short message. %u\n", msg->len);
		goto exit_msgbfree;
	}

	switch (l1h->msg_type) {
	case L1CTL_FBSB_REQ:
		l1ctl_rx_fbsb_req(msg);
		break;
	case L1CTL_DM_EST_REQ:
		l1ctl_rx_dm_est_req(msg);
		break;
	case L1CTL_DM_REL_REQ:
		l1ctl_rx_dm_rel_req(msg);
		break;
	case L1CTL_PARAM_REQ:
		l1ctl_rx_param_req(msg);
		break;
	case L1CTL_DM_FREQ_REQ:
		l1ctl_rx_dm_freq_req(msg);
		break;
	case L1CTL_CRYPTO_REQ:
		l1ctl_rx_crypto_req(msg);
		break;
	case L1CTL_RACH_REQ:
		l1ctl_rx_rach_req(msg);
		break;
	case L1CTL_DATA_REQ:
		l1ctl_rx_data_req(msg);
		/* we have to keep the msgb, not free it! */
		goto exit_nofree;
	case L1CTL_PM_REQ:
		l1ctl_rx_pm_req(msg);
		break;
	case L1CTL_RESET_REQ:
		l1ctl_rx_reset_req(msg);
		break;
	case L1CTL_CCCH_MODE_REQ:
		l1ctl_rx_ccch_mode_req(msg);
		break;
	case L1CTL_TCH_MODE_REQ:
		l1ctl_rx_tch_mode_req(msg);
		break;
	}

exit_msgbfree:
	msgb_free(msg);
exit_nofree:
	return;
}

void l1a_l23api_init(void)
{
	sercomm_register_rx_cb(SC_DLCI_L1A_L23, l1a_l23_rx_cb);
}
