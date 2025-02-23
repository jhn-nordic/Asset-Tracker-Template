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

// Add global socket variable
static int ping_sock = -1;

// New function to open socket
int open_ping_socket(void)
{
	if (ping_sock >= 0) {
		LOG_DBG("Ping socket already open");
		return 0;
	}

	ping_sock = socket(AF_PACKET, SOCK_RAW, 0);
	if (ping_sock < 0) {
		LOG_ERR("Failed to create socket: %d", -errno);
		return -1;
	}

	// // Set the receive timeout to 30 seconds
	// struct timeval tv = {
	// 	.tv_sec = 30,
	// 	.tv_usec = 0,
	// };
	// if (setsockopt(ping_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
	// 	LOG_ERR("Failed to set socket timeout: %d", -errno);
	// 	close(ping_sock);
	// 	ping_sock = -1;
	// 	return -1;
	// }

	LOG_DBG("Ping socket opened successfully: %d", ping_sock);
	return 0;
}

// New function to close socket
void close_ping_socket(void)
{
	if (ping_sock >= 0) {
		close(ping_sock);
		LOG_DBG("Ping socket closed: %d", ping_sock);
		ping_sock = -1;
	
	}
}

// Modify perform_ping to use the global socket
int64_t perform_ping(void)
{
	const char *target = "8.8.8.8";
	int ret;
	int64_t result = -1;
	struct sockaddr_in dest_addr = {
		.sin_family = AF_INET,
		.sin_port = 0  // Not used for ICMP
	};

	// Check if socket is open
	if (ping_sock < 0) {
		LOG_ERR("Ping socket not open");
		return -1;
	}

	// Declare these variables early since we'll use them for both flushing and receiving
	struct sockaddr_storage addr;
	socklen_t addr_len = sizeof(addr);
	
	// Flush the socket by reading any pending data
	uint8_t flush_buf[128];
	int flags = MSG_DONTWAIT;  // Non-blocking receive
	
	while (recvfrom(ping_sock, flush_buf, sizeof(flush_buf), flags,
					(struct sockaddr *)&addr, &addr_len) > 0) {
		// Continue reading until no more data is available
	}

	// Convert the target IP directly
	if (inet_pton(AF_INET, target, &dest_addr.sin_addr) != 1) {
		LOG_ERR("Failed to convert target IP address");
		return -1;
	}

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
		return -1;
	}

	// Convert IP string to bytes
	struct in_addr src_ip;
	if (inet_pton(AF_INET, src_addr, &src_ip) != 1) {
		LOG_ERR("Failed to convert source IP address");
		return -1;
	}
	memcpy(buf + 12, &src_ip.s_addr, 4);

	// Destination IP address (8.8.8.8)
	memcpy(buf + 16, &dest_addr.sin_addr.s_addr, 4);

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
	LOG_INF("Sending ping to %s", target);
	ret = send(ping_sock, buf, sizeof(buf), 0);
	if (ret < 0) {
		LOG_ERR("send() failed: %d", -errno);
		return -1;
	}

	uint8_t recv_buf[64];
	LOG_INF("Receiving ping from %s", target);
	ret = recvfrom(ping_sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&addr, &addr_len);
	if (ret < 0) {
		LOG_ERR("recvfrom() failed or timed out: %d", -errno);
		return -1;
	}

	// Check if the received packet is an ICMP Echo Reply
	if (ret >= (IP_HDR_LEN + ICMP_HDR_LEN)) {
		uint8_t *icmp_reply = recv_buf + IP_HDR_LEN;
		if (icmp_reply[0] == 0) { // ICMP Echo Reply
			result = k_uptime_delta(&start_time);
			LOG_INF("Ping reply from %s: RTT = %lld ms", target, result);
		}
		else {
			LOG_ERR("Received packet is not an ICMP Echo Reply");
		}
	}
	else {
		LOG_ERR("Received packet is not an ICMP Echo Reply");
	}

	return result;
} 