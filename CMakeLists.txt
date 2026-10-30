#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

if(NOT CONFIG_ATT_MODULES)
	return()
endif()

zephyr_include_directories(common)

# Modules include each other's headers even when the included module is disabled,
# for example storage including location.h.
target_include_directories(app PRIVATE
	modules/button
	modules/cloud
	modules/environmental
	modules/fota
	modules/led
	modules/location
	modules/network
	modules/power
	modules/storage
	modules/uart_power_control
)

target_sources(app PRIVATE common/app_watchdog.c)

add_subdirectory(modules/network)
add_subdirectory(cbor)

add_subdirectory_ifdef(CONFIG_APP_BUTTON modules/button)
add_subdirectory_ifdef(CONFIG_APP_POWER modules/power)
add_subdirectory_ifdef(CONFIG_APP_ENVIRONMENTAL modules/environmental)
add_subdirectory_ifdef(CONFIG_APP_LED modules/led)
add_subdirectory_ifdef(CONFIG_APP_LOCATION modules/location)
add_subdirectory_ifdef(CONFIG_APP_CLOUD modules/cloud)
add_subdirectory_ifdef(CONFIG_APP_FOTA modules/fota)
add_subdirectory_ifdef(CONFIG_APP_STORAGE modules/storage)
add_subdirectory_ifdef(CONFIG_APP_UART_POWER_CONTROL modules/uart_power_control)
