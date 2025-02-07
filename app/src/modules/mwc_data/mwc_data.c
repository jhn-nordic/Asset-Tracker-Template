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
#if defined(CONFIG_APP_BATTERY)
#include "battery.h"

#endif


#if defined(CONFIG_APP_BATTERY)
static struct battery_msg latest_battery_msg;
static bool battery_data_valid = false;
#endif

static struct environmental_msg latest_env_msg;
static bool env_data_valid = false;

/* Register log module */
LOG_MODULE_REGISTER(mwc_data, CONFIG_APP_LOG_LEVEL);

/* Define a ZBUS listener for this module */
static void mwc_data_callback(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(mwc_data_listener, mwc_data_callback);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, mwc_data_listener, 0);
ZBUS_CHAN_ADD_OBS(TRIGGER_CHAN, mwc_data_listener, 0);
#if defined(CONFIG_APP_BATTERY)
ZBUS_CHAN_ADD_OBS(BATTERY_CHAN, mwc_data_listener, 0);
#endif
ZBUS_CHAN_ADD_OBS(ENVIRONMENTAL_CHAN, mwc_data_listener, 0);

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

/* New definitions for thread and message queue */
#define MWC_DATA_THREAD_STACK_SIZE 4096

/* Define an event structure for CLOUD and TRIGGER events */
struct mwc_data_event {
	const struct zbus_channel *chan;
	union {
		enum cloud_msg_type cloud_status;
		enum trigger_type trigger;
	} data;
};

/* Define a message queue for passing events to the thread */
K_MSGQ_DEFINE(mwc_data_msgq, sizeof(struct mwc_data_event), 10, 4);



/* Updated state object: Added trigger field */
static struct state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	enum cloud_msg_type status;
	/* New field to store trigger events */
	enum trigger_type trigger;
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
		/* Instead of reading directly from the channel message, use the stored trigger value */
		if (user_object->trigger == TRIGGER_MWC_DATA) {
			LOG_INF("Received MWC_DATA trigger, performing ping test");
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

	#if defined(CONFIG_APP_BATTERY)
			double battery_val = battery_data_valid ? latest_battery_msg.percentage : 0.0;
	#endif
    #if defined(CONFIG_APP_ENVIRONMENTAL)
			double temp = env_data_valid ? latest_env_msg.temperature : 0.0;
			double pressure = env_data_valid ? latest_env_msg.pressure : 0.0;
			double humidity = env_data_valid ? latest_env_msg.humidity : 0.0;
    #endif

			/* Build the JSON output including battery and environmental data */
			struct cloud_payload payload = {0};
			payload.buffer_len = snprintf((char *)payload.buffer,
				sizeof(payload.buffer),
				"{\"ping\": %lld, \"rsrp\": \"%s\", \"band\": \"%s\", \"ue_mode\": \"%s\", \"operator\": \"%s\""
	#if defined(CONFIG_APP_BATTERY)
				", \"battery\": %.2f"
	#endif
    #if defined(CONFIG_APP_ENVIRONMENTAL)
				", \"temp\": %.2f, \"pressure\": %.2f, \"humidity\": %.2f"
    #endif
				"}",
				ping_rtt, rsrp, band, ue_mode, oper
	#if defined(CONFIG_APP_BATTERY)
				, battery_val
	#endif
    #if defined(CONFIG_APP_ENVIRONMENTAL)
				, temp, pressure, humidity
    #endif
				);

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
#if defined(CONFIG_APP_BATTERY)
	if (chan == &BATTERY_CHAN) {

		const struct battery_msg *msg = zbus_chan_const_msg(chan);
		latest_battery_msg = *msg;  // store the battery reading
		battery_data_valid = true;
		LOG_DBG("Updated battery data: percentage=%.2f", msg->percentage);

		return;
	}
#endif
	if (chan == &ENVIRONMENTAL_CHAN) {
		const struct environmental_msg *msg = zbus_chan_const_msg(chan);
		latest_env_msg = *msg;  // store the environmental reading
		env_data_valid = true;
		LOG_DBG("Updated environmental data: temp=%.2f, pressure=%.2f, humidity=%.2f",
			msg->temperature, msg->pressure, msg->humidity);
		return;
	}

	/* For CLOUD and TRIGGER channels, enqueue the event */
	struct mwc_data_event event;
	event.chan = chan;
	if (chan == &CLOUD_CHAN) {
		const enum cloud_msg_type *status = zbus_chan_const_msg(chan);
		event.data.cloud_status = *status;
	} else if (chan == &TRIGGER_CHAN) {
		const enum trigger_type *trigger = zbus_chan_const_msg(chan);
		event.data.trigger = *trigger;
	} else {
		LOG_ERR("Unknown channel: %s", zbus_chan_name(chan));
		return;
	}

	LOG_DBG("Enqueueing message from channel %s", zbus_chan_name(chan));
	err = k_msgq_put(&mwc_data_msgq, &event, K_NO_WAIT);
	if (err) {
		LOG_ERR("Message queue full, dropping event from channel %s", zbus_chan_name(chan));
	}
}

/* Dedicated thread for processing mwc_data events */
static void mwc_data_thread(void *arg1, void *arg2, void *arg3)
{
    	STATE_SET_INITIAL(mwc_data_state, STATE_INIT);
	while (1) {
		struct mwc_data_event event;
		/* Block until an event is available */
		if (k_msgq_get(&mwc_data_msgq, &event, K_FOREVER) == 0) {
			/* Update the state object based on the queued event */
			mwc_data_state.chan = event.chan;
			if (event.chan == &CLOUD_CHAN) {
				mwc_data_state.status = event.data.cloud_status;
			} else if (event.chan == &TRIGGER_CHAN) {
				mwc_data_state.trigger = event.data.trigger;
			}
			LOG_DBG("Processing event from channel %s", zbus_chan_name(event.chan));
			int err = STATE_RUN(mwc_data_state);
			if (err) {
				LOG_ERR("smf_run_state, error: %d", err);
				SEND_FATAL_ERROR();
			}
		}
	}
}


K_THREAD_DEFINE(mwc_data_module_thread_id,
		MWC_DATA_THREAD_STACK_SIZE,
		mwc_data_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);