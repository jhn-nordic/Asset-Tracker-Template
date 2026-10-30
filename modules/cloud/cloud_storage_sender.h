/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_STORAGE_SENDER_H_
#define _CLOUD_STORAGE_SENDER_H_

#include <stddef.h>
#include <zephyr/sys/iterable_sections.h>

#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Structure to define how the cloud module sends one storage data type */
struct cloud_storage_sender {
	/* Storage type id of the items to send */
	enum storage_data_type type;

	/* Size of the stored data type. Items of another size are dropped. */
	size_t data_size;

	/**
	 * @brief Function to send one stored item to the cloud
	 *
	 * Called in the cloud module thread while the cloud is connected, for each item of a
	 * storage batch or a storage flush.
	 *
	 * @param data Pointer to the stored data. Will be cast to the data type internally.
	 * @param len Size of the stored data, equal to data_size.
	 *
	 * @retval 0 if the item was sent. The item is removed from storage.
	 * @retval -EINVAL or -ENOTSUP if the item cannot be sent. The item is removed from storage.
	 * @retval Other negative errno if sending failed. The item stays in storage, and the
	 *         batch is sent again after the cloud reconnects.
	 */
	int (*send)(const void *data, size_t len);
};

/**
 * @brief Register how the cloud module sends a storage data type.
 *
 * The macro can be used in any file that is built into the application. Register the data
 * type itself with STORAGE_DATA_TYPE_ADD().
 *
 * Example usage:
 * @code
 * CLOUD_STORAGE_SENDER_DEFINE(STORAGE_TYPE_BATTERY,  - Storage type id
 *                             struct power_msg,      - Type of the stored data
 *                             battery_send);         - Function to send one item
 * @endcode
 *
 * @param _id Storage type id, a decimal literal or a macro that expands to one
 * @param _data_type Type of the stored data
 * @param _send_fn Function that sends one item, see struct cloud_storage_sender
 */
#define CLOUD_STORAGE_SENDER_DEFINE(_id, _data_type, _send_fn)					\
	/* The id in the symbol name makes duplicate ids fail to link */			\
	const STRUCT_SECTION_ITERABLE(cloud_storage_sender,					\
				      CONCAT(cloud_storage_sender_, _id)) = {			\
		.type = _id,									\
		.data_size = sizeof(_data_type),						\
		.send = _send_fn,								\
	}

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_STORAGE_SENDER_H_ */
