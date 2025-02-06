/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/net/socket.h>
#include <stdio.h>
#include <string.h>
#include <nrf_socket.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/unistd.h>
#include <modem/modem_info.h>

#include "modules_common.h"
#include "message_channel.h"
#include "ping.h"
#include "../cloud/cloud_module.h"
#include <modem/modem_info.h>


/* Register log module */
LOG_MODULE_REGISTER(mwc_data, CONFIG_APP_LOG_LEVEL);

/* Define a ZBUS listener for this module */
static void mwc_data_callback(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(mwc_data_listener, mwc_data_callback);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, mwc_data_listener, 0);
ZBUS_CHAN_ADD_OBS(TRIGGER_CHAN, mwc_data_listener, 0);

/* Forward declarations */
static const struct smf_state states[];

/* State machine definitions */
static const struct smf_state states[];

/* Forward declarations of state handlers */
static void init_entry(void *o);
static void init_run(void *o);
static void cloud_connected_entry(void *o);
static void cloud_connected_run(void *o);
static void cloud_disconnected_entry(void *o);
static void cloud_disconnected_run(void *o);

/* Define states */
enum state {
	STATE_INIT,
	STATE_CLOUD_CONNECTED,
	STATE_CLOUD_DISCONNECTED,
};

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_INIT] = SMF_CREATE_STATE(
		init_entry,
		init_run,
		NULL,
		NULL,
		NULL
	),
	[STATE_CLOUD_CONNECTED] = SMF_CREATE_STATE(
		cloud_connected_entry,
		cloud_connected_run,
		NULL,
		NULL,
		NULL
	),
	[STATE_CLOUD_DISCONNECTED] = SMF_CREATE_STATE(
		cloud_disconnected_entry,
		cloud_disconnected_run,
		NULL,
		NULL,
		NULL
	)
};

/* State object */
static struct state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	enum cloud_msg_type status;
} mwc_data_state;

/* State implementations */
static void init_entry(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);
}

static void init_run(void *o)
{
	struct state_object *user_object = o;

	if (user_object->chan == &CLOUD_CHAN) {
		if (user_object->status == CLOUD_CONNECTED_READY_TO_SEND) {
			LOG_DBG("Cloud connected and ready, going into connected state");
			STATE_SET(mwc_data_state, STATE_CLOUD_CONNECTED);
			return;
		}

		if ((user_object->status == CLOUD_DISCONNECTED) ||
		    (user_object->status == CLOUD_CONNECTED_PAUSED)) {
			LOG_DBG("Cloud disconnected/paused, going into disconnected state");
			STATE_SET(mwc_data_state, STATE_CLOUD_DISCONNECTED);
			return;
		}
	}
}

static void cloud_connected_entry(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);
}


static void cloud_connected_run(void *o)
{
	struct state_object *user_object = o;

	if ((user_object->chan == &CLOUD_CHAN) &&
	    ((user_object->status == CLOUD_CONNECTED_PAUSED) ||
	     (user_object->status == CLOUD_DISCONNECTED))) {
		LOG_DBG("Cloud disconnected/paused, going into disconnected state");
		STATE_SET(mwc_data_state, STATE_CLOUD_DISCONNECTED);
		return;
	}

	if (user_object->chan == &TRIGGER_CHAN) {
		const enum trigger_type *trigger = zbus_chan_const_msg(user_object->chan);
		if (*trigger == TRIGGER_MWC_DATA) {
			LOG_INF("Received MWC_DATA trigger, performing ping test");
			/* Call the modified ping function and obtain the round-trip time */
			int64_t ping_rtt = perform_ping();

			/* Collect modem info values */
			char rsrp[16] = {0}, band[16] = {0}, ue_mode[16] = {0}, oper[16] = {0};
			int ret;
			ret = modem_info_string_get(MODEM_INFO_RSRP, rsrp, sizeof(rsrp));
			if (ret < 0) {
				snprintf(rsrp, sizeof(rsrp), "N/A");
			}
			ret = modem_info_string_get(MODEM_INFO_CUR_BAND, band, sizeof(band));
			if (ret < 0) {
				snprintf(band, sizeof(band), "N/A");
			}
			ret = modem_info_string_get(MODEM_INFO_UE_MODE, ue_mode, sizeof(ue_mode));
			if (ret < 0) {
				snprintf(ue_mode, sizeof(ue_mode), "N/A");
			}
			ret = modem_info_string_get(MODEM_INFO_OPERATOR, oper, sizeof(oper));
			if (ret < 0) {
				snprintf(oper, sizeof(oper), "N/A");
			}

			/* Build the JSON output */
			struct cloud_payload payload = {0};
			payload.buffer_len = snprintf((char *)payload.buffer, sizeof(payload.buffer),
			    "{\"ping\": %lld, \"rsrp\": %s, \"band\": %s, \"ue_mode\": %s, \"operator\": \"%s\"}",
			    ping_rtt, rsrp, band, ue_mode, oper);
			LOG_INF("Output JSON: %s", payload.buffer);

			/* Publish the JSON payload to the PAYLOAD channel */
			zbus_chan_pub(&PAYLOAD_CHAN, &payload,  K_SECONDS(1));
		}
	}
}

static void cloud_disconnected_entry(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);
}

static void cloud_disconnected_run(void *o)
{
	struct state_object *user_object = o;

	if ((user_object->chan == &CLOUD_CHAN) &&
	    (user_object->status == CLOUD_CONNECTED_READY_TO_SEND)) {
		LOG_DBG("Cloud connected and ready, going into connected state");
		STATE_SET(mwc_data_state, STATE_CLOUD_CONNECTED);
		return;
	}
}

/* Callback for handling channel messages */
static void mwc_data_callback(const struct zbus_channel *chan)
{
	int err;

	if ((chan != &CLOUD_CHAN) && (chan != &TRIGGER_CHAN)) {
		LOG_ERR("Unknown channel");
		return;
	}

	LOG_DBG("Received message on channel %s", zbus_chan_name(chan));

	mwc_data_state.chan = chan;

	if (chan == &CLOUD_CHAN) {
		const enum cloud_msg_type *status = zbus_chan_const_msg(chan);
		mwc_data_state.status = *status;
	} else if (chan == &TRIGGER_CHAN) {
		/* Handle trigger when implemented */
		LOG_DBG("Received trigger");
	}

	err = STATE_RUN(mwc_data_state);
	if (err) {
		LOG_ERR("smf_run_state, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

/* Module initialization */
static int mwc_data_init(void)
{
	STATE_SET_INITIAL(mwc_data_state, STATE_INIT);
	return 0;
}

SYS_INIT(mwc_data_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);