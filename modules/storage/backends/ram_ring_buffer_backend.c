/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "storage.h"
#include "storage_backend.h"
#include "storage_data_types.h"

LOG_MODULE_DECLARE(storage, CONFIG_APP_STORAGE_LOG_LEVEL);

#define RECORDS_PER_TYPE	CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE

/* RAM backend context */
struct ram_backend_ctx {
	/* Number of data types registered */
	int num_registered_types;
};

static struct ram_backend_ctx ctx;

/**
 * @brief Initialize the RAM storage backend
 *
 * Counts the number of registered data types and ensures it doesn't exceed
 * the configured maximum. Also calculates and logs the total RAM usage.
 *
 * @return 0 on success, negative errno on failure
 */
static int ram_init(void)
{
	/* Count registered types */
	STRUCT_SECTION_FOREACH(storage_data, t) {
		ctx.num_registered_types++;

		LOG_DBG("RAM backend initialized with %d types, using %zu bytes of RAM",
			ctx.num_registered_types,
			t->data_size * (size_t)RECORDS_PER_TYPE);

		LOG_DBG("Ring buffer %s initialized with size %u, item size: %zu", t->name,
			ring_buf_capacity_get(t->ram_ring_buf), t->data_size);
	}

	/* Ensure we don't exceed configured maximum */
	__ASSERT(ctx.num_registered_types <= CONFIG_APP_STORAGE_MAX_TYPES,
		 "Too many storage types registered (%d). "
		 "Increase CONFIG_APP_STORAGE_MAX_TYPES.",
		 ctx.num_registered_types);

	return 0;
}

/**
 * @brief Store data in the RAM backend
 *
 * Stores data in the ring buffer associated with the given data type.
 * If the ring buffer is full, the oldest data will be overwritten.
 *
 * @param type Storage data type to store data for
 * @param data Pointer to the data to store
 * @param size Size of the data in bytes
 *
 * @return 0 on success, negative errno on failure
 */
static int ram_store(const struct storage_data *type, const void *data, size_t size)
{
	struct ring_buf *ring_buf;
	uint32_t bytes_written;

	if (!type || !data) {
		return -EINVAL;
	}

	ring_buf = type->ram_ring_buf;

	LOG_DBG("%s ring_buf: %p, data size: %zu (%zu), free: %u bytes",
		type->name, ring_buf, size, type->data_size, ring_buf_space_get(ring_buf));

	/* Check if we need to remove old data to make space */
	if (ring_buf_space_get(ring_buf) < (uint32_t)size) {
		LOG_DBG("Full buffer, old data will be overwritten");

		/* Remove the oldest record in the ring buffer */
		bytes_written = ring_buf_get(ring_buf, NULL, type->data_size);
		if (bytes_written != (uint32_t)type->data_size) {
			LOG_ERR("Failed to discard oldest record data");
			return -EIO;
		}

		LOG_DBG("Removed oldest record of size %zu", type->data_size);
	}

	/* Store the actual data */
	bytes_written = ring_buf_put(ring_buf, data, (uint32_t)size);
	if (bytes_written != (uint32_t)size) {
		LOG_ERR("Failed to write all data");
		return -EIO;
	}

	LOG_DBG("Stored %s item, count: %u, left: %u bytes",
		type->name, storage_backend_get()->count(type),
		ring_buf_space_get(ring_buf));

	return 0;
}

/**
 * @brief Peek at data from the RAM backend without removing it
 *
 * Returns the size of the next item without copying data (if data is NULL)
 * or copies the data if data buffer is provided.
 *
 * @param type Storage data type to peek at
 * @param data Pointer where the peeked data will be stored (can be NULL for size-only)
 * @param size Size of the data buffer in bytes (ignored if data is NULL)
 * @return Number of bytes that would be read on success, negative errno on failure
 */
static int ram_peek(const struct storage_data *type, void *data, size_t size)
{
	struct ring_buf *ring_buf;
	uint32_t bytes_peeked;

	if (!type) {
		return -EINVAL;
	}

	ring_buf = type->ram_ring_buf;

	if (ring_buf_is_empty(ring_buf)) {
		return -EAGAIN;
	}

	/* If data is NULL, just return the size without copying */
	if (data == NULL) {
		return (int)type->data_size;
	}

	/* Data buffer provided - ensure it's large enough */
	if (type->data_size > size) {
		LOG_ERR("Buffer too small for data: needed %zu, have %zu", type->data_size, size);

		return -ENOMEM;
	}

	/* Peek at the data without removing it */
	bytes_peeked = ring_buf_peek(ring_buf, data, (uint32_t)type->data_size);
	if (bytes_peeked != (uint32_t)type->data_size) {
		LOG_ERR("Failed to peek data: expected %zu bytes, got %u",
			type->data_size, bytes_peeked);

		return -EIO;
	}

	return (int)bytes_peeked;
}

/**
 * @brief Retrieve data from the RAM backend
 *
 * Retrieves the oldest stored data for the given data type from its ring buffer.
 *
 * @param type Storage data type to retrieve data for
 * @param data Pointer where the retrieved data will be stored
 * @param size Size of the data buffer in bytes
 * @return Number of bytes read on success, negative errno on failure
 */
static int ram_retrieve(const struct storage_data *type, void *data, size_t size)
{
	size_t bytes_read;
	struct ring_buf *ring_buf;

	if (!type || !data) {
		return -EINVAL;
	}

	ring_buf = type->ram_ring_buf;

	if (ring_buf_is_empty(ring_buf)) {
		return -EAGAIN;
	}

	/* Ensure buffer is large enough */
	if (type->data_size > size) {
		LOG_ERR("Buffer too small for data: needed %zu, have %zu", type->data_size, size);
		return -ENOMEM;
	}

	/* Read the data */
	bytes_read = ring_buf_get(ring_buf, data, (uint32_t)type->data_size);
	if (bytes_read != (uint32_t)type->data_size) {
		LOG_ERR("Failed to read data: expected %zu bytes, got %u",
			type->data_size, bytes_read);
		return -EIO;
	}

	LOG_DBG("Retrieved item in %s ring buffer, size: %u bytes, %u items left",
		type->name, bytes_read, storage_backend_get()->count(type));

	return (int)bytes_read;
}

/**
 * @brief Count the number of records stored for a data type
 *
 * Calculates how many records are currently stored in the ring buffer
 * for the given data type.
 *
 * @param type Storage data type to count records for
 * @return Number of records on success, negative errno on failure
 */
static int ram_records_count(const struct storage_data *type)
{
	const struct ring_buf *ring_buf;
	size_t bytes_used;
	size_t item_count;

	if (!type) {
		return -EINVAL;
	}

	ring_buf = type->ram_ring_buf;

	if (ring_buf_is_empty(ring_buf)) {
		return 0;
	}

	/* Calculate bytes used (total capacity minus available space) */
	bytes_used = ring_buf_capacity_get(ring_buf) - ring_buf_space_get(ring_buf);

	/* Calculate the number of items (bytes used divided by the size of each item) */
	item_count = bytes_used / type->data_size;

	LOG_DBG("Counted %zu items in %s ring buffer", item_count, type->name);

	return (int)item_count;
}

/**
 * @brief Clear all stored data in the RAM backend
 *
 * Resets all ring buffers to their empty state using the Zephyr ring buffer API,
 * clearing any stored data while preserving the ring buffer context structure.
 *
 * @return 0 on success
 * @retval -EINVAL if no types are registered
 */
static int ram_clear(void)
{
	if (ctx.num_registered_types == 0) {
		return -EINVAL;
	}

	/* Iterate through all registered types */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		ring_buf_reset(type->ram_ring_buf);
	}

	return 0;
}

/**
 * @brief RAM storage backend interface
 *
 * Implementation of the storage_backend interface for RAM-based storage.
 * Provides functions for initializing the backend, storing and retrieving
 * data, counting stored records, and clearing all data.
 */
static const struct storage_backend ram_backend = {
	.init = ram_init,
	.store = ram_store,
	.peek = ram_peek,
	.retrieve = ram_retrieve,
	.count = ram_records_count,
	.clear = ram_clear,
};

/**
 * @brief Get the RAM storage backend interface
 *
 * Makes the RAM storage backend available to the storage module.
 *
 * @return Pointer to the RAM storage backend interface
 */
const struct storage_backend *storage_backend_get(void)
{
	return &ram_backend;
}
