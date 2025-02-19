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
#include <nrf_modem_at.h>
#include <nrf_errno.h>
#include <nrf_modem_gnss.h>
#include <modem/lte_lc.h>
#include "../network/network.h"

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
static void gnss_active_entry(void *o);
static void gnss_active_run(void *o);

/* Forward declarations of GNSS functions */
static int gnss_init(void);
static int gnss_start(void);
static int gnss_stop(void);
static void gnss_event_handler(int event);

/* Define states */
enum state {
	STATE_INIT,
	STATE_CLOUD_CONNECTED,
	STATE_CLOUD_DISCONNECTED,
	STATE_GNSS_ACTIVE,  // New state for GNSS operations
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
	),
	[STATE_GNSS_ACTIVE] = SMF_CREATE_STATE(gnss_active_entry, gnss_active_run, NULL, NULL, NULL),
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



/* Updated state object: Added trigger field and altitude field */
static struct state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	enum cloud_msg_type status;
	/* New field to store trigger events */
	enum trigger_type trigger;
	bool gnss_fix_valid;         // Track if we got a valid fix
	double latitude;             // Store latitude from fix
	double longitude;            // Store longitude from fix
	uint32_t gnss_timeout_ms;   // Track time spent waiting for fix
	double altitude;             // Store altitude from fix (not sent to cloud)
	float accuracy;             // Changed from uint32_t to float to match GNSS API
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

	if (user_object->chan == &TRIGGER_CHAN) {
		if (user_object->trigger == TRIGGER_GNSS_START) {
			LOG_INF("GNSS start triggered");
			STATE_SET(mwc_data_state, STATE_GNSS_ACTIVE);
			return;
		}
	}

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
		if (user_object->trigger == TRIGGER_GNSS_START) {
			LOG_INF("GNSS start triggered");
			STATE_SET(mwc_data_state, STATE_GNSS_ACTIVE);
			return;
		}
		if (user_object->trigger == TRIGGER_MWC_DATA) {
			LOG_INF("Received MWC_DATA trigger, performing ping test");
			int64_t ping_rtt = perform_ping();

			/* Collect modem info values */
			char rsrp[16] = {0}, band[16] = {0}, ue_mode[16] = {0}, oper[16] = {0}, imei[16] = {0};
			int ret;
			
			ret = modem_info_string_get(MODEM_INFO_IMEI, imei, sizeof(imei));
			if (ret < 0) {
				snprintf(imei, sizeof(imei), "N/A");
			}
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

			/* Build the output payload including battery and environmental data */
			struct cloud_payload payload = {0};
#if defined(CONFIG_APP_MWC_DATA_CSV)
			/* Prepare CSV output with 13 columns:
			 * imei, ping, rsrp, band, ue_mode, operator, latitude, longitude, accuracy, battery, temp, pressure, humidity
			 */
#if defined(CONFIG_APP_BATTERY)
			char battery_str[16] = "";

			if (battery_data_valid) {
				snprintf(battery_str, sizeof(battery_str), "%.2f", battery_val);
			}
#endif

			// Add buffer declarations for lat/lon string conversion
			char lat_buf[32] = "";
			char lon_buf[32] = "";

#if defined(CONFIG_APP_ENVIRONMENTAL)
			char temp_str[16] = "", pressure_str[16] = "", humidity_str[16] = "";

			if (env_data_valid) {
				snprintf(temp_str, sizeof(temp_str), "%.2f", temp);
				snprintf(pressure_str, sizeof(pressure_str), "%.2f", pressure);
				snprintf(humidity_str, sizeof(humidity_str), "%.2f", humidity);
			}
#endif

			/* Use GNSS fix data or configured values based on Kconfig */
#if defined(CONFIG_APP_USE_GNSS_FIX)
			const char *lat_str = mwc_data_state.gnss_fix_valid ? 
				snprintf(lat_buf, sizeof(lat_buf), "%.6f", mwc_data_state.latitude) >= 0 ? lat_buf : "" : "";
			const char *lon_str = mwc_data_state.gnss_fix_valid ? 
				snprintf(lon_buf, sizeof(lon_buf), "%.6f", mwc_data_state.longitude) >= 0 ? lon_buf : "" : "";
			int accuracy = mwc_data_state.gnss_fix_valid ? 
				mwc_data_state.accuracy : 0;
#else
			const char *lat_str = CONFIG_APP_MWC_DATA_LATITUDE;
			const char *lon_str = CONFIG_APP_MWC_DATA_LONGITUDE;
			int accuracy = CONFIG_APP_MWC_DATA_GPS_ACCURACY;
#endif

			payload.buffer_len = snprintf((char *)payload.buffer,
				sizeof(payload.buffer),
				"%s,,%lld,%s,%s,%s,%s,%s,%s,%d,%s,%s,%s,%s",
				imei,
				ping_rtt,
				rsrp,
				band,
				ue_mode,
				oper,
				lat_str,
				lon_str,
				accuracy,
#if defined(CONFIG_APP_BATTERY)
				battery_str,
#else
				"",
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
				temp_str,
				pressure_str,
				humidity_str
#else
				"", "", ""
#endif
			);
#else
			/* JSON payload */
			payload.buffer_len = snprintf((char *)payload.buffer,
				sizeof(payload.buffer),
				"{\"id\": \"%s\", \"ping\": %lld, \"rsrp\": \"%s\", \"band\": \"%s\", \"ue_mode\": \"%s\", \"operator\": \"%s\", "
#if defined(CONFIG_APP_USE_GNSS_FIX)
				"\"latitude\": \"%.*f\", \"longitude\": \"%.*f\", \"accuracy\": %d"
#else
				"\"latitude\": \"%s\", \"longitude\": \"%s\", \"accuracy\": %d"
#endif
#if defined(CONFIG_APP_BATTERY)
				", \"battery\": %.2f"
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
				", \"temp\": %.2f, \"pressure\": %.2f, \"humidity\": %.2f"
#endif
				"}",
				imei,
				ping_rtt,
				rsrp,
				band,
				ue_mode,
				oper,
#if defined(CONFIG_APP_USE_GNSS_FIX)
				6, mwc_data_state.gnss_fix_valid ? mwc_data_state.latitude : 0.0,
				6, mwc_data_state.gnss_fix_valid ? mwc_data_state.longitude : 0.0,
				mwc_data_state.gnss_fix_valid ? mwc_data_state.accuracy : 0
#else
				CONFIG_APP_MWC_DATA_LATITUDE,
				CONFIG_APP_MWC_DATA_LONGITUDE,
				CONFIG_APP_MWC_DATA_GPS_ACCURACY
#endif
#if defined(CONFIG_APP_BATTERY)
				, battery_val
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
				, temp, pressure, humidity
#endif
			);
#endif

			LOG_INF("Output payload: %s", payload.buffer);

			/* Publish the payload to the PAYLOAD channel */
			zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
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

if (user_object->chan == &TRIGGER_CHAN) {
		if (user_object->trigger == TRIGGER_GNSS_START) {
			LOG_INF("GNSS start triggered");
			STATE_SET(mwc_data_state, STATE_GNSS_ACTIVE);
			return;
		}
	}

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

int setup_NTN_modem_commands(void) {
    int err;

    /* Set modem to minimum functionality */
    err = nrf_modem_at_printf(CONFIG_APP_NTN_AT_CFUN);
    if (err) {
        LOG_ERR("Failed to set CFUN, error: %d", err);
        return err;
    }

    /* Set APN for data connection */
    err = nrf_modem_at_printf(CONFIG_APP_NTN_AT_CGDCONT);
    if (err) {
        LOG_ERR("Failed to set CGDCONT, error: %d", err);
        return err;
    }

    /* Set system mode to NB-IoT only */
    err = nrf_modem_at_printf(CONFIG_APP_NTN_AT_SYSTEMMODE);
    if (err) {
        LOG_ERR("Failed to set XSYSTEMMODE, error: %d", err);
        return err;
    }

    err = nrf_modem_at_printf(CONFIG_APP_NTN_AT_EPCO);
    if (err) {
        LOG_ERR("Failed to set XEPCO, error: %d", err);
        return err;
    }

    /* Set GPS position */
	if (CONFIG_APP_USE_GNSS_FIX) {
		int latitude = (int)(mwc_data_state.latitude * 1000)+90000;
		int longitude = (int)(mwc_data_state.longitude * 1000)+180000;
		int altitude = (int)(mwc_data_state.altitude * 1000);
		err = nrf_modem_at_printf("AT%%XSETGPSPOS=%d,%d,%d",longitude,latitude,altitude);
    
	} else {
		err = nrf_modem_at_printf(CONFIG_APP_NTN_AT_SETGPSPOS);
	}
    if (err) {
        LOG_ERR("Failed to set XSETGPSPOS, error: %d", err);
        return err;
    }

    /* Configure NTN features */
    err = nrf_modem_at_printf(CONFIG_APP_NTN_AT_NTNFEAT);
    if (err) {
        LOG_ERR("Failed to set XNTNFEAT, error: %d", err);
        return err;
    }

    /* Set band lock */
    err = nrf_modem_at_printf(CONFIG_APP_NTN_AT_BANDLOCK);
    if (err) {
        LOG_ERR("Failed to set XBANDLOCK, error: %d", err);
        return err;
    }

    LOG_INF("NTN modem configuration completed successfully");
    return 0;
}

K_THREAD_DEFINE(mwc_data_module_thread_id,
		MWC_DATA_THREAD_STACK_SIZE,
		mwc_data_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/* Add these defines from the sample */
#define PI 3.14159265358979323846
#define EARTH_RADIUS_METERS (6371.0 * 1000.0)

/* Add event handler declarations */
static void gnss_event_handler(int event);
static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static K_SEM_DEFINE(pvt_data_sem, 0, 1);
static K_SEM_DEFINE(gnss_fix_sem, 0, 1);

/* Update the GNSS event handler */
static void gnss_event_handler(int event)
{
    int retval;

    switch (event) {
    case NRF_MODEM_GNSS_EVT_PVT:
        retval = nrf_modem_gnss_read(&last_pvt, sizeof(last_pvt), NRF_MODEM_GNSS_DATA_PVT);
        if (retval == 0) {
            LOG_INF("PVT data - Timestamp: %02d:%02d:%02d, Date: %02d-%02d-%04d",
                   last_pvt.datetime.hour, last_pvt.datetime.minute, 
                   last_pvt.datetime.seconds,
                   last_pvt.datetime.day, last_pvt.datetime.month, 
                   last_pvt.datetime.year);
            
            // Fix: Use correct field names from the nRF GNSS API
            uint8_t tracked = 0;
            uint8_t in_fix = 0;
            
            // Count satellites that are tracked and used in fix
            for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; ++i) {
                if (last_pvt.sv[i].sv > 0) {
                    tracked++;
                    if (last_pvt.sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
                        in_fix++;
                    }
                }
            }
            
            LOG_INF("Satellites tracked: %d, Satellites in fix: %d",
                   tracked, in_fix);
				   
            if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
                // Store the fix data in state object
                mwc_data_state.gnss_fix_valid = true;
                mwc_data_state.latitude = last_pvt.latitude;
                mwc_data_state.longitude = last_pvt.longitude;
                mwc_data_state.altitude = last_pvt.altitude;
                mwc_data_state.accuracy = last_pvt.accuracy;
                
                LOG_INF("GNSS fix obtained - Lat: %.06f, Lon: %.06f, Alt: %.1f m, Accuracy: %.1f m", 
                       (double)last_pvt.latitude, (double)last_pvt.longitude, 
                       (double)last_pvt.altitude, (double)last_pvt.accuracy);
                
                k_sem_give(&gnss_fix_sem);
            }
        }
        break;

    default:
        break;
    }
}

/* Add GNSS initialization function */
static int gnss_init(void)
{
    /* Enable GNSS */
    if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS) != 0) {
        LOG_ERR("Failed to activate GNSS functional mode");
        return -1;
    }

    /* Configure GNSS */
    if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0) {
        LOG_ERR("Failed to set GNSS event handler");
        return -1;
    }

    /* Set fix interval to single fix */
    if (nrf_modem_gnss_fix_interval_set(0) != 0) {
        LOG_ERR("Failed to set GNSS fix interval");
        return -1;
    }

    /* Set fix retry to 0 for single fix */
    if (nrf_modem_gnss_fix_retry_set(0) != 0) {
        LOG_ERR("Failed to set GNSS fix retry");
        return -1;
    }

    return 0;
}

/* Add GNSS start function */
static int gnss_start(void)
{
    if (nrf_modem_gnss_start() != 0) {
        LOG_ERR("Failed to start GNSS");
        return -1;
    }
    return 0;
}

/* Add GNSS stop function */
static int gnss_stop(void)
{
    if (nrf_modem_gnss_stop() != 0) {
        LOG_ERR("Failed to stop GNSS");
        return -1;
    }
    return 0;
}

/* Add new state handler for GNSS operations */
static void gnss_active_entry(void *o)
{
    struct state_object *user_object = o;
    
    // Initialize GNSS state
    user_object->gnss_fix_valid = false;
    user_object->gnss_timeout_ms = 0;
    
    // Initialize and start GNSS
    if (gnss_init() != 0) {
        LOG_ERR("Failed to initialize GNSS");
        STATE_SET(mwc_data_state, STATE_CLOUD_CONNECTED);
        return;
    }
    
    if (gnss_start() != 0) {
        LOG_ERR("Failed to start GNSS");
        STATE_SET(mwc_data_state, STATE_CLOUD_CONNECTED);
        return;
    }
    
    LOG_INF("GNSS started");
}

/* Update the gnss_active_run function */
static void gnss_active_run(void *o)
{
    ARG_UNUSED(o);  // Add this to explicitly mark the parameter as unused
    
    // Use a timeout when waiting for the semaphore
    if (k_sem_take(&gnss_fix_sem, K_SECONDS(360)) == 0) {
        // We got a fix, clean up GNSS
        gnss_stop();

        // Transition back to disconnected state
        STATE_SET(mwc_data_state, STATE_CLOUD_DISCONNECTED);

        // Now it's safe to send the network connect message
        struct network_msg msg = {
            .type = NETWORK_CONNECT
        };
        int err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
        if (err) {
            LOG_ERR("Failed to publish network connect message, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
        LOG_DBG("Published network connect message");
    }
}