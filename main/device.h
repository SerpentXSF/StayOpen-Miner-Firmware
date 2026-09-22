#ifndef _DEVICE_H
#define _DEVICE_H

#include "global_state.h"

#include "gpio_input_output.h"
#include "internal_sensor.h"
#include "hal_i2c.h"
#include "EMC2302.h"
#include "TMP75.h"
#include "TPS546.h"
#include "pwm_fan.h"
#include "ethernet_init.h"


esp_err_t read_hash_board_temperature(GlobalState *GLOBAL_STATE);
int       read_power_temp(void);

/*
 * The highest core voltage this model may be commanded to, in hundredths of
 * a volt. Ask before setting a voltage from anywhere.
 */
uint16_t  device_core_voltage_ceiling(GlobalState * GLOBAL_STATE);
esp_err_t read_power_information(GlobalState *GLOBAL_STATE);

/* Thermal sensor addresses for a device model. `fallback` is the model to
 * adopt if the sensor is absent, or NULL where the address does not
 * distinguish this model from another and reconfiguring would only cause a
 * boot loop. */
esp_err_t device_thermal_addresses(DeviceModel model, uint8_t *primary,
                                   uint8_t *secondary, const char **fallback);
esp_err_t power_on_hashboard(GlobalState *GLOBAL_STATE);
esp_err_t power_off_hashboard(GlobalState *GLOBAL_STATE);
void      reset_hash_board(GlobalState *GLOBAL_STATE);

/* True for the boards that drive the fan from LEDC rather than an EMC2302,
 * and take their hashboard supply through the USB-PD gate. */
bool device_is_bc01_family(DeviceModel model);

/* Negotiate the USB-PD supply and open the VBUS gate. Everything on the
 * hashboard -- the fan and the core regulator included -- is unpowered until
 * this succeeds, so anything that measures them has to run after it. */
esp_err_t bc01_pd_bringup(GlobalState *GLOBAL_STATE);

esp_err_t set_fan_pwm(GlobalState *GLOBAL_STATE, uint8_t pwm_percent);
esp_err_t read_fan_rpm(GlobalState *GLOBAL_STATE);

esp_err_t init_all_peripherals(GlobalState *GLOBAL_STATE);

void dev_display_init(GlobalState *GLOBAL_STATE);

#ifdef STATISTIC_SYSTEM_FEATURE
esp_err_t statistic_init_system_by_device(GlobalState *GLOBAL_STATE);
#endif

#endif