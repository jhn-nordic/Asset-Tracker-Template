/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SOIL_SENSOR_H_
#define _SOIL_SENSOR_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	soil_sensor_chan
);

enum soil_sensor_msg_type {
	/* Output message types */

	/* Response message to a request for current soil sensor values.
	 * The sampled values are found in the respective fields of the message structure.
	 */
	SOIL_SENSOR_SAMPLE_RESPONSE = 0x1,

	/* Input message types */

	/* Request to sample the current soil sensor values.
	 * The response is sent as a SOIL_SENSOR_SAMPLE_RESPONSE message.
	 */
	SOIL_SENSOR_SAMPLE_REQUEST,
};

struct soil_sensor_msg {
	enum soil_sensor_msg_type type;

	/** Volumetric soil moisture in percent, 0 to 100. */
	uint8_t moisture_pct;

	/** Soil temperature in tenths of a degree Celsius. */
	int16_t temperature_dc;

	/** Timestamp when the sample was taken in milliseconds.
	 *  This is either:
	 * - Unix time in milliseconds if the system clock was synchronized at sampling time, or
	 * - Uptime in milliseconds if the system clock was not synchronized at sampling time.
	 */
	int64_t timestamp;
};

#ifdef __cplusplus
}
#endif

#endif /* _SOIL_SENSOR_H_ */
