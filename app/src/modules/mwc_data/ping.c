/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <stdio.h>
#include <string.h>
#include <nrf_socket.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/unistd.h>
#include <modem/modem_info.h>

#include "ping.h"

LOG_MODULE_REGISTER(ping, CONFIG_APP_LOG_LEVEL);

/* Helper function to calculate the standard ICMP checksum */
static uint16_t calculate_checksum(const void *buf, int len)
{
	const uint16_t *data = buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *data++;
		len -= 2;
	}
	if (len == 1) {
		uint16_t last_byte = 0;
		memcpy(&last_byte, data, 1);
		sum += last_byte;
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return ~((uint16_t)sum);
}

int64_t perform_ping(void)
{
	const char *target = "8.8.8.8";
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	int ret;
	int sock = -1;
	int64_t result = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;          // IPv4 address
	hints.ai_socktype = SOCK_RAW;
	hints.ai_protocol = IPPROTO_ICMP;

	ret = getaddrinfo(target, NULL, &hints, &res);
	if (ret != 0) {
		LOG_ERR("getaddrinfo() failed: %d", ret);
		goto cleanup;
	}

	sock = socket(AF_PACKET, SOCK_RAW, 0);
	if (sock < 0) {
		LOG_ERR("Failed to create socket: %d", -errno);
		goto cleanup;
	}

	// Set the receive timeout to 30 seconds
	struct timeval tv = {
		.tv_sec = 30,
		.tv_usec = 0,
	};
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	// Build the IPv4 + ICMP Echo Request packet
	#define IP_HDR_LEN 20
	#define ICMP_HDR_LEN 8
	#define TOTAL_LEN (IP_HDR_LEN + ICMP_HDR_LEN)

	uint8_t buf[TOTAL_LEN];
	memset(buf, 0, sizeof(buf));

	// IPv4 header
	buf[0] = (4 << 4) + (IP_HDR_LEN / 4);  // Version & header length
	buf[2] = TOTAL_LEN >> 8;                // Total length
	buf[3] = TOTAL_LEN & 0xFF;              // Total length
	buf[8] = 64;                            // TTL
	buf[9] = IPPROTO_ICMP;                  // Protocol

	// Get source IP address using modem_info
	char src_addr[NET_IPV4_ADDR_LEN] = {0};
	ret = modem_info_string_get(MODEM_INFO_IP_ADDRESS, src_addr, NET_IPV4_ADDR_LEN);
	if (ret < 0) {
		LOG_ERR("Failed to get source IP address: %d", ret);
		goto cleanup;
	}

	// Convert IP string to bytes
	struct in_addr src_ip;
	if (inet_pton(AF_INET, src_addr, &src_ip) != 1) {
		LOG_ERR("Failed to convert source IP address");
		goto cleanup;
	}
	memcpy(buf + 12, &src_ip.s_addr, 4);

	// Destination IP address
	struct sockaddr_in *dest = (struct sockaddr_in *)res->ai_addr;
	memcpy(buf + 16, &dest->sin_addr.s_addr, 4);

	// Calculate IPv4 header checksum
	buf[10] = 0;
	buf[11] = 0;
	uint16_t ipv4_checksum = calculate_checksum(buf, IP_HDR_LEN);
	buf[10] = ipv4_checksum & 0xFF;
	buf[11] = ipv4_checksum >> 8;

	// ICMP header
	uint8_t *icmp = buf + IP_HDR_LEN;
	icmp[0] = 8;  // Echo Request
	icmp[4] = 0;  // ID high byte
	icmp[5] = 1;  // ID low byte
	icmp[6] = 0;  // Sequence high byte
	icmp[7] = 1;  // Sequence low byte

	// Calculate ICMP checksum
	uint16_t icmp_checksum = calculate_checksum(icmp, ICMP_HDR_LEN);
	icmp[2] = icmp_checksum & 0xFF;
	icmp[3] = icmp_checksum >> 8;

	int64_t start_time = k_uptime_get();
	ret = send(sock, buf, sizeof(buf), 0);
	if (ret < 0) {
		LOG_ERR("send() failed: %d", -errno);
		goto cleanup;
	}

	uint8_t recv_buf[64];
	struct sockaddr_storage addr;
	socklen_t addr_len = sizeof(addr);
	ret = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&addr, &addr_len);
	if (ret < 0) {
		LOG_ERR("recvfrom() failed or timed out: %d", -errno);
		goto cleanup;
	}

	// Check if the received packet is an ICMP Echo Reply
	if (ret >= (IP_HDR_LEN + ICMP_HDR_LEN)) {
		uint8_t *icmp_reply = recv_buf + IP_HDR_LEN;
		if (icmp_reply[0] == 0) { // ICMP Echo Reply
			result = k_uptime_delta(&start_time);
			LOG_INF("Ping reply from %s: RTT = %lld ms", target, result);
		}
	}

cleanup:
	if (sock >= 0) {
		close(sock);
	}
	if (res != NULL) {
		freeaddrinfo(res);
	}
	return result;
} 