/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>

#include "app_common.h"
#include "soil_sensor.h"

/* Register log module */
LOG_MODULE_REGISTER(soil_sensor, CONFIG_APP_SOIL_SENSOR_LOG_LEVEL);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(soil_sensor_chan,
		 struct soil_sensor_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(soil_sensor);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(soil_sensor_chan, soil_sensor, 0);

#define MAX_MSG_SIZE sizeof(struct soil_sensor_msg)

#define MOISTURE_PCT_MIN	0
#define MOISTURE_PCT_MAX	100
#define MOISTURE_PCT_STEP_MAX	2
#define TEMPERATURE_DC_MIN	-100
#define TEMPERATURE_DC_MAX	400
#define TEMPERATURE_DC_STEP_MAX	5

BUILD_ASSERT(CONFIG_APP_SOIL_SENSOR_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_SOIL_SENSOR_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* State machine */

/* Soil sensor module states.
 */
enum soil_sensor_module_state {
	/* The module is running and waiting for sensor value requests */
	STATE_RUNNING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct soil_sensor_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last zbus message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Last mock sensor values */
	int moisture_pct;
	int temperature_dc;
};

/* Forward declarations of state handlers */
static enum smf_state_result state_running_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(NULL, state_running_run, NULL, NULL, NULL),
};

static int random_step(int step_max)
{
	return (int)(sys_rand32_get() % (2 * step_max + 1)) - step_max;
}

static void sample_sensors(struct soil_sensor_state_object *state_object)
{
	int err;

	state_object->moisture_pct = CLAMP(state_object->moisture_pct +
					   random_step(MOISTURE_PCT_STEP_MAX),
					   MOISTURE_PCT_MIN, MOISTURE_PCT_MAX);
	state_object->temperature_dc = CLAMP(state_object->temperature_dc +
					     random_step(TEMPERATURE_DC_STEP_MAX),
					     TEMPERATURE_DC_MIN, TEMPERATURE_DC_MAX);

	struct soil_sensor_msg msg = {
		.type = SOIL_SENSOR_SAMPLE_RESPONSE,
		.moisture_pct = (uint8_t)state_object->moisture_pct,
		.temperature_dc = (int16_t)state_object->temperature_dc,
	};

	LOG_DBG("Moisture: %d %%, Temperature: %d dC", msg.moisture_pct, msg.temperature_dc);

	err = zbus_chan_pub(&soil_sensor_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void soil_sensor_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* State handlers */

static enum smf_state_result state_running_run(void *obj)
{
	struct soil_sensor_state_object *state_object = obj;

	if (&soil_sensor_chan == state_object->chan) {
		const struct soil_sensor_msg *msg =
			(const struct soil_sensor_msg *)state_object->msg_buf;

		if (msg->type == SOIL_SENSOR_SAMPLE_REQUEST) {
			LOG_DBG("Soil sensor sample request received, getting data");
			sample_sensors(state_object);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void soil_sensor_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_SOIL_SENSOR_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_SOIL_SENSOR_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	/* Place state object in .data section to ensure it is captured in coredumps
	 * and can be inspected by external tools during state analysis.
	 */
	__attribute__((section(".data")))
	static struct soil_sensor_state_object soil_sensor_state = {
		.moisture_pct = 40,
		.temperature_dc = 150,
	};

	LOG_DBG("Soil sensor module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, soil_sensor_wdt_callback,
				   (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&soil_sensor_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&soil_sensor,
					&soil_sensor_state.chan,
					soil_sensor_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&soil_sensor_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(soil_sensor_module_thread_id,
		CONFIG_APP_SOIL_SENSOR_THREAD_STACK_SIZE,
		soil_sensor_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
