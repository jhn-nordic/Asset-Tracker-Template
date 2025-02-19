/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef MWC_DATA_H__
#define MWC_DATA_H__

/**
 * @brief MWC data module that handles collecting and sending modem data.
 * @defgroup mwc_data MWC Data module
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Setup NTN modem commands
 *
 * Configures the modem with NTN-specific AT commands defined in Kconfig
 *
 * @return 0 on success, negative error code on failure
 */
int setup_NTN_modem_commands(void);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* MWC_DATA_H__ */ 