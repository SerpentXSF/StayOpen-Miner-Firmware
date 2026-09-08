#include <stdbool.h>

#include "esp_log.h"
#include "esp_check.h"

#include "miner.h"
#include "hal_i2c.h"

#include "EMC2302.h"

static const char * TAG = "EMC2302";

static i2c_master_dev_handle_t EMC2302_dev_handle;

esp_err_t EMC2302_init(void)
{
	return bc_i2c_add_device(EMC2302_ADDR, &EMC2302_dev_handle, TAG);
}

esp_err_t EMC2302_installed(bool Polarity)
{
	esp_err_t err;

	ESP_LOGI(TAG, "initializing EMC2302 : %d", Polarity);

    // set polarity of ch1 and ch2
    err = bc_i2c_register_write_byte(EMC2302_dev_handle, EMC2302_POLARITY, (Polarity) ? 0x03 : 0x00);
    if (err != ESP_OK) {
        return err;
    }

    // set output type to push pull of ch1 and ch2
    err = bc_i2c_register_write_byte(EMC2302_dev_handle, EMC2302_OUTPUT_CONFIG, 0x03);
    if (err != ESP_OK) {
        return err;
    }

    // set base frequency of ch1 and ch2 to 19.53kHz
    err = bc_i2c_register_write_byte(EMC2302_dev_handle, EMC2302_BASE_F123, (0x01 << 0) | (0x01 << 3));
    if (err != ESP_OK) {
        return err;
    }

    // manual fan control
    // bits 4-3: 0b01 = 5 edge samples (2 poles)
    err = bc_i2c_register_write_byte(EMC2302_dev_handle, EMC2302_FAN1 + EMC2302_OFS_FAN_CONFIG1, (0b01 << 3));
    if (err != ESP_OK) {
        return err;
    }

    err = bc_i2c_register_write_byte(EMC2302_dev_handle, EMC2302_FAN2 + EMC2302_OFS_FAN_CONFIG1, (0b01 << 3));
    if (err != ESP_OK) {
        return err;
    }

    return err;
}


esp_err_t EMC2302_set_fan_polarity(bool invert) 
{
    // set polarity of ch1 and ch2
    return bc_i2c_register_write_byte(EMC2302_dev_handle, EMC2302_POLARITY, (invert) ? 0x03 : 0x00);
}

esp_err_t EMC2302_set_fan_speed(uint8_t percent)
{
    int value = (int) (255.0 * percent / 100 + 0.5);
    value = (value > 255) ? 255 : value;

    esp_err_t err;

    //ESP_LOGI(TAG, "setting fan speed to %d", percent);

    err = bc_i2c_register_write_byte(EMC2302_dev_handle, EMC2302_FAN1 + EMC2302_OFS_FAN_SETTING, (uint8_t) value);
    if (err != ESP_OK) {
        return err;
    }
    err = bc_i2c_register_write_byte(EMC2302_dev_handle, EMC2302_FAN2 + EMC2302_OFS_FAN_SETTING, (uint8_t) value);
    return err;
}

esp_err_t EMC2302_get_fan_speed1(uint16_t *dst)
{
    esp_err_t err;
    uint8_t tach_lsb, tach_msb;

    // use channel 1 
    err = bc_i2c_register_read(EMC2302_dev_handle, EMC2302_FAN1 + EMC2302_OFS_TACH_READING_MSB, &tach_msb, 1);
    if (err != ESP_OK) {
        *dst = 0;
        return err;
    }
    err = bc_i2c_register_read(EMC2302_dev_handle, EMC2302_FAN1 + EMC2302_OFS_TACH_READING_LSB, &tach_lsb, 1);
    if (err != ESP_OK) {
        *dst = 0;
        return err;
    }

    //ESP_LOGI(TAG, "Raw Fan Speed1 = %02X %02X", tach_msb, tach_lsb);

    // 3 LSBs are unused
    int rpm_raw = (tach_msb << 5) | (tach_lsb >> 3) ;

    const int poles = 2;
    const int n = 5; // number of edges measured (typically five for a two-pole fan)

    // tested with NF-A9x14
    // the tacho is about 87Hz at 100%, noctua says 2 cycles per revolution
    // and the emc2302 gives 1487 as raw value
    // rpm = 87Hz * 60 / 2 = 2610
    // rpm = 60 * 32768 * 1 * (5 - 1) / 2 / 1487 = 2644
    const int m = 1; // the multiplier defined by the RANGE bits

    const int ftach = 32768;

    int rpm = 60 * ftach * m * (n - 1) / poles / rpm_raw;

    //ESP_LOGI(TAG, "raw fan speed: %d", rpm_raw);

    // we get this if no fan is connected
    // would be displayed as 480RPM
    // so we actually can't measure lower than that
    // but the datasheet says the measurement range is
    // 480 to 16000RPM. So it seems to be fine.
    if (rpm_raw >= 8191) {
        rpm = 0;
    }

    if (rpm > 65535) {
        ESP_LOGE(TAG, "fan speed RPM > 16bit: %d", rpm);
        // on invalid result set it to 0 to indicate an error
        rpm = 0;
    } else {
        ESP_LOGD(TAG, "fan speed: %dRPM", rpm);
    }

    *dst = rpm;
    return ESP_OK;
}

esp_err_t EMC2302_get_fan_speed2(uint16_t *dst)
{
    esp_err_t err;
    uint8_t tach_lsb, tach_msb;

    // use channel 2 
    err = bc_i2c_register_read(EMC2302_dev_handle, EMC2302_FAN2 + EMC2302_OFS_TACH_READING_MSB, &tach_msb, 1);
    if (err != ESP_OK) {
        *dst = 0;
        return err;
    }
    err = bc_i2c_register_read(EMC2302_dev_handle, EMC2302_FAN2 + EMC2302_OFS_TACH_READING_LSB, &tach_lsb, 1);
    if (err != ESP_OK) {
        *dst = 0;
        return err;
    }

    //ESP_LOGI(TAG, "Raw Fan Speed2 = %02X %02X", tach_msb, tach_lsb);

    // 3 LSBs are unused
    int rpm_raw = (tach_msb << 5) | (tach_lsb >> 3) ;

    const int poles = 2;
    const int n = 5; // number of edges measured (typically five for a two-pole fan)

    // tested with NF-A9x14
    // the tacho is about 87Hz at 100%, noctua says 2 cycles per revolution
    // and the emc2302 gives 1487 as raw value
    // rpm = 87Hz * 60 / 2 = 2610
    // rpm = 60 * 32768 * 1 * (5 - 1) / 2 / 1487 = 2644
    const int m = 1; // the multiplier defined by the RANGE bits

    const int ftach = 32768;

    int rpm = 60 * ftach * m * (n - 1) / poles / rpm_raw;

    //ESP_LOGI(TAG, "raw fan speed: %d", rpm_raw);

    // we get this if no fan is connected
    // would be displayed as 480RPM
    // so we actually can't measure lower than that
    // but the datasheet says the measurement range is
    // 480 to 16000RPM. So it seems to be fine.
    if (rpm_raw >= 8191) {
        rpm = 0;
    }

    if (rpm > 65535) {
        ESP_LOGE(TAG, "fan speed RPM > 16bit: %d", rpm);
        // on invalid result set it to 0 to indicate an error
        rpm = 0;
    } else {
        ESP_LOGD(TAG, "fan speed: %dRPM", rpm);
    }

    *dst = rpm;
    return ESP_OK;
}

esp_err_t EMC2302_get_fan_speed(uint16_t *dst)
{
    uint16_t rpm1 = 0;
    uint16_t rpm2 = 0;

    /*
     * Say when the reads failed.
     *
     * Both return values used to be dropped and ESP_OK returned regardless,
     * so a controller that had stopped answering reported a healthy zero --
     * indistinguishable from a fan that had stopped turning, and both were
     * then ignored anyway. A caller that cannot tell "no fan" from "no
     * controller" cannot protect the board from either.
     *
     * One good channel is still an answer: these boards drive two outputs and
     * a single dead channel leaves a usable reading. Only when neither
     * responds is there nothing to report.
     */
    esp_err_t err1 = EMC2302_get_fan_speed1(&rpm1);
    esp_err_t err2 = EMC2302_get_fan_speed2(&rpm2);

    if (ESP_OK != err1 && ESP_OK != err2) {
        *dst = 0;
        return err1;
    }

    *dst = rpm1 > rpm2 ? rpm1 : rpm2;
    return ESP_OK;
}