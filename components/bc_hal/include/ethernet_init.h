/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#pragma once

#include "esp_eth_driver.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Ethernet driver based on Espressif IoT Development Framework Configuration
 *
 * @param[out] eth_handles_out array of initialized Ethernet driver handles
 * @param[out] eth_cnt_out number of initialized Ethernets
 * @return
 *          - ESP_OK on success
 *          - ESP_ERR_INVALID_ARG when passed invalid pointers
 *          - ESP_ERR_NO_MEM when there is no memory to allocate for Ethernet driver handles array
 *          - ESP_FAIL on any other failure
 */
esp_err_t example_eth_init(esp_eth_handle_t *eth_handles_out[], uint8_t *eth_cnt_out);

/**
 * @brief Hold the Ethernet controller in hardware reset
 *
 * Takes the W5500 out of the path of the core-rail transient that powers the
 * hashboard, rather than merely keeping the driver away from it. Call before
 * the rail is switched on; example_eth_init() releases it.
 *
 * @return
 *          - ESP_OK when the part is held in reset
 *          - the gpio error otherwise, with the controller left live
 */
esp_err_t eth_phy_hold_in_reset(void);

/**
 * @brief Release a controller held by eth_phy_hold_in_reset()
 *
 * Does nothing if it was never held. Called by example_eth_init(); there is no
 * need to call it directly.
 */
void eth_phy_release_reset(void);

#ifdef __cplusplus
}
#endif
