/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef PING_H_
#define PING_H_

#include <stdint.h>

/**
 * @brief Perform a ping test to 8.8.8.8
 *
 * Sends an ICMP Echo Request to 8.8.8.8 and waits for a response.
 * Returns the round-trip time (RTT) in milliseconds if a reply is received,
 * otherwise returns -1.
 */
int64_t perform_ping(void);

#endif /* PING_H_ */ 