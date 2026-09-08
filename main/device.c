#include <stdbool.h>

#include "device.h"
#include "vcore.h"
#include "nvs_config.h"
#include "miner.h"
#include "system.h"
#include "eeprom.h"
#include "asic.h"
#include "lvgl_porting.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "main.h"
#include "HUSB238A.h"
#include "hal_i2c.h"
#include "pwm_fan.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

#define TAG "device"

/*
 * Per-model I2C thermal sensor layout.
 *
 * Some boards cannot be told apart by their sensor address alone: BC01,
 * BC02 and BC04 all answer at 0x48. Where a board IS distinguishable, a
 * missing sensor is treated as evidence the configured model is wrong and
 * the firmware reconfigures itself and reboots. That is what `fallback`
 * expresses; models with no distinguishing address leave it NULL and only
 * log, because guessing would trade a wrong reading for a boot loop.
 *
 * The vendor release carried BC04, BC06 and BC08 as three near-identical
 * copies of the same block, and sent everything else -- including BC01 and
 * BC02, which nvs_device.c already configures -- down an else branch that
 * reset the model to "DC04". No such model exists in nvs_device.c, so a
 * BC01 running that build would reset, fail to match again, and loop.
 */
typedef struct {
    DeviceModel  model;
    const char  *name;
    uint8_t      primary_addr;
    uint8_t      secondary_addr;   /* 0 when the board has one sensor */
    const char  *fallback;         /* model to adopt if the sensor is absent */
} device_thermal_profile_t;

static const device_thermal_profile_t device_thermal_profiles[] = {
    { DEVICE_BC01,     "BC01",     TMP75_I2CADDR_BC01,     0, "BC01-Pro" },
    { DEVICE_BC01_Pro, "BC01-Pro", TMP75_I2CADDR_BC01_Pro, 0, "BC01"     },
    { DEVICE_BC02,     "BC02",     TMP75_I2CADDR_BC01,     0, NULL       },
    { DEVICE_BC04, "BC04", TMP75_I2CADDR_BC04,   0,                    "BC08" },
    { DEVICE_BC06, "BC06", TMP75_I2CADDR_BC08_1, TMP75_I2CADDR_BC08_2, "BC04" },
    { DEVICE_BC08, "BC08", TMP75_I2CADDR_BC08_1, TMP75_I2CADDR_BC08_2, "BC04" },
};

static const device_thermal_profile_t *find_thermal_profile(DeviceModel model)
{
    for (size_t i = 0; i < sizeof(device_thermal_profiles) / sizeof(device_thermal_profiles[0]); i++) {
        if (device_thermal_profiles[i].model == model) {
            return &device_thermal_profiles[i];
        }
    }
    return NULL;
}

/* Lets self_test.c use the same table rather than keeping its own copy. */

/*
 * USB Power Delivery bring-up for the BC01 family.
 *
 * These boards take their hashboard supply through a HUSB238A PD sink
 * controller: the adapter is negotiated up from the USB default, and only
 * then is VBUS gated through. Until that happens the TPS546 core regulator
 * and the EMC2302 fan controller have no power and do not answer on I2C at
 * all, so the ASIC cannot be started.
 *
 * The BC04 has no PD stage, so the BC04 source release contains none of
 * this. The sequence below is imported from the BC01 source published at
 * github.com/baichuan-org/BC01 (commit 8dab8f4, main/device.c), kept close
 * to the original so it can be diffed against it. See docs/BC01-USB-PD.md.
 *
 * Note what the negotiated current is used for: the ASIC frequency and
 * voltage ceilings are derated from it, so a weaker adapter runs the chip
 * slower rather than browning out. That relationship is not guessable and
 * is the main reason this was worth importing rather than reimplementing.
 */
static void pd_power_io_off(void)
{
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << CONFIG_GPIO_OVERHEATE_ALERT_0),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&gpio_conf);
    gpio_set_level(CONFIG_GPIO_OVERHEATE_ALERT_0, 1);
}

static void pd_power_io_on(void)
{
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << CONFIG_GPIO_OVERHEATE_ALERT_0),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&gpio_conf);
    gpio_set_level(CONFIG_GPIO_OVERHEATE_ALERT_0, 0);
}

bool device_is_bc01_family(DeviceModel model)
{
    return model == DEVICE_BC01 || model == DEVICE_BC02 || model == DEVICE_BC01_Pro;
}

/*
 * Negotiating twice is not harmful but it is not free either: the ladder walks
 * the adapter down to 5 V and back up, which is a second of reduced supply for
 * no gain once a contract is already in place. Callers can therefore ask as
 * often as they like; only the first successful negotiation does any work.
 *
 * Only set on success, so a failed attempt is retried by the next caller
 * rather than being remembered as done.
 */
static bool pd_contract_held = false;

esp_err_t bc01_pd_bringup(GlobalState *GLOBAL_STATE)
{
    if (pd_contract_held) {
        ESP_LOGD(TAG, "USB-PD contract already held; nothing to negotiate");
        return ESP_OK;
    }


    // Status Variables
    bool attached = false, sink_ready = false;
    uint8_t negotiated_volt = 0;
    uint8_t fault = 0;
    husb238_protocol_t protocol = PROTOCOL_NONE;
    uint16_t time_out = 0;

    // Dynamic voltage list (only supported voltages)
    uint8_t volt_list[5] = {0};
    uint8_t volt_count = 0;
    uint8_t curr_index = 0;
    uint8_t target_volt = 0;

    bool voltage_setting = false;
    bool set_success = false;
    bool cap_loaded = false;

    // Driver initialization
    ESP_ERROR_CHECK(husb238a_init());
    //vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "PD Negotiation: Read adapter capability first, 5-15V");

    while (1) {
        // Read PD status
        if(attached == false ||  sink_ready == false)
        {
            husb238a_get_connect_status(&attached, &sink_ready);
        }
        else
        {
            if(protocol == PROTOCOL_NONE)
            {
                protocol = husb238_get_protocol();
            }
            husb238a_get_negotiated_voltage(&negotiated_volt);
            husb238a_get_fault(&fault);
        }

        // Print status every 200ms
        if ((time_out % 20) == 0) {
            ESP_LOGI(TAG, "attached=%d, ready=%d, volt=%dV, fault=%d, target=%dV",
                    attached, sink_ready, negotiated_volt, fault, target_volt);
        }

        // Safe rule: Only operate when attached, ready and voltage > 0V
        if (attached && sink_ready && protocol)
        {
            // Read adapter capability once
            if (!cap_loaded) 
            {
                bool sup5, sup9, sup12, sup15;
                vTaskDelay(pdMS_TO_TICKS(50));
                husb238a_get_capability(&sup5, &sup9, &sup12, &sup15);
                
                // Build voltage list (Priority: 5V → 9V → 12V → 15V)
                volt_count = 0;
                if(sup5)  volt_list[volt_count++] = 5;
                if(sup9)  volt_list[volt_count++] = 9;
                if(sup12) volt_list[volt_count++] = 12;
                if(sup15) volt_list[volt_count++] = 15;

                if(volt_count == 0){
                    ESP_LOGE(TAG, "No supported voltage detected");
                    break;
                }

                target_volt = volt_list[curr_index];
                ESP_LOGI(TAG, "Adapter supported voltages:");
                for(int i=0; i<volt_count; i++){
                    ESP_LOGI(TAG, "%dV", volt_list[i]);
                }

                cap_loaded = true;
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Check if negotiation success
            if (negotiated_volt == target_volt) {
                ESP_LOGI(TAG, "Negotiation success: %dV", target_volt);
                set_success = true;
                if (curr_index >= volt_count -1) 
                {
                    break;
                }
                else
                {
                    curr_index++;
                    target_volt = volt_list[curr_index];
                    time_out = 0;
                    voltage_setting = false;
                    husb238a_gate_open();
                    pd_power_io_on();
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
            }

            // Set target voltage if not setting
            if (negotiated_volt != 0 && !voltage_setting && target_volt != 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                husb238a_set_voltage(target_volt);
                ESP_LOGI(TAG, "Set voltage: %dV", target_volt);
                voltage_setting = true;
                time_out = 0;
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Timeout handler, switch to next voltage
            if (time_out > 200) {
                ESP_LOGW(TAG, "%dV negotiation timeout", target_volt);
                curr_index++;
                if (curr_index >= volt_count) {
                    ESP_LOGE(TAG, "All voltage attempts failed");
                    break;
                }
                target_volt = volt_list[curr_index];
                voltage_setting = false;
                time_out = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        time_out++;

        // Global timeout protection
        if(time_out > 500){
            ESP_LOGW(TAG, "Global negotiation timeout");
            break;
        }
    }

    //husb238_print_info();
    // Check negotiated current
    if(set_success)
    {
        uint16_t curr = husb238_get_negotiated_current();
        ESP_LOGI(TAG, "CONTRACT CURRENT %d.",curr);
        if(negotiated_volt == 15)
        {
            if(curr < 2000)
            {
                set_success = false;
            }
        }
        else if(negotiated_volt == 12)
        {
            if(curr < 3000)
            {
                set_success = false;
            }
        }
        else if(negotiated_volt == 9)
        {
            if(curr >= 3000)
            {
                if(DEVICE_BC01_Pro == GLOBAL_STATE->device_model)
                    GLOBAL_STATE->asic_freqency = GLOBAL_STATE->asic_freqency < 350 ? GLOBAL_STATE->asic_freqency : 350;
                else if(DEVICE_BC01 == GLOBAL_STATE->device_model)
                    GLOBAL_STATE->asic_freqency = GLOBAL_STATE->asic_freqency < 650 ? GLOBAL_STATE->asic_freqency : 650;
            }
            else if(curr >= 2770)
            {
                if(DEVICE_BC01_Pro == GLOBAL_STATE->device_model)
                    GLOBAL_STATE->asic_freqency = GLOBAL_STATE->asic_freqency < 330 ? GLOBAL_STATE->asic_freqency : 330;
                else if(DEVICE_BC01 == GLOBAL_STATE->device_model)
                    GLOBAL_STATE->asic_freqency = GLOBAL_STATE->asic_freqency < 600 ? GLOBAL_STATE->asic_freqency : 600;

            }
            else if(curr >= 2220)
            {
                if(DEVICE_BC01_Pro == GLOBAL_STATE->device_model)
                    GLOBAL_STATE->asic_freqency = GLOBAL_STATE->asic_freqency < 300 ? GLOBAL_STATE->asic_freqency : 300;
                else if(DEVICE_BC01 == GLOBAL_STATE->device_model)
                    GLOBAL_STATE->asic_freqency = GLOBAL_STATE->asic_freqency < 480 ? GLOBAL_STATE->asic_freqency : 480;

            }
            else
            {
                set_success = false;
            }
            if(DEVICE_BC01_Pro == GLOBAL_STATE->device_model)
                GLOBAL_STATE->asic_vol_default = GLOBAL_STATE->asic_vol_default < 95 ? GLOBAL_STATE->asic_vol_default : 95;
            else if(DEVICE_BC01 == GLOBAL_STATE->device_model)
                GLOBAL_STATE->asic_vol_default = GLOBAL_STATE->asic_vol_default < 115 ? GLOBAL_STATE->asic_vol_default : 115;
        }
        else
        {
            if(curr < 3000)
            {
                set_success = false;
            }
            else
            {
                if(DEVICE_BC01_Pro == GLOBAL_STATE->device_model)
                    GLOBAL_STATE->asic_freqency = GLOBAL_STATE->asic_freqency < 300 ? GLOBAL_STATE->asic_freqency : 300;
                else if(DEVICE_BC01 == GLOBAL_STATE->device_model)
                    GLOBAL_STATE->asic_freqency = GLOBAL_STATE->asic_freqency < 400 ? GLOBAL_STATE->asic_freqency : 400;
            }
            if(DEVICE_BC01_Pro == GLOBAL_STATE->device_model)
                GLOBAL_STATE->asic_vol_default = GLOBAL_STATE->asic_vol_default < 93 ? GLOBAL_STATE->asic_vol_default : 93;
            else if(DEVICE_BC01 == GLOBAL_STATE->device_model)
                GLOBAL_STATE->asic_vol_default = GLOBAL_STATE->asic_vol_default < 115 ? GLOBAL_STATE->asic_vol_default : 115;
        }

        if(set_success == false)
        {
            ESP_LOGW(TAG, "The current is too weak to function properly.");
        }
    }

    // VBUS control
    if (set_success) {
        vTaskDelay(pdMS_TO_TICKS(500));
        husb238a_gate_open();
        pd_power_io_on();
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "VBUS output enabled");
    } else {
        ESP_LOGE(TAG, "VBUS output disabled");
    }

    if (set_success) {
        GLOBAL_STATE->pd_state = 1;
        pd_contract_held = true;
        return ESP_OK;
    }

    GLOBAL_STATE->pd_state = 0;
    return ESP_FAIL;
}

esp_err_t device_thermal_addresses(DeviceModel model, uint8_t *primary,
                                   uint8_t *secondary, const char **fallback)
{
    const device_thermal_profile_t *profile = find_thermal_profile(model);
    if (NULL == profile) {
        return ESP_ERR_NOT_FOUND;
    }
    if (primary)   *primary   = profile->primary_addr;
    if (secondary) *secondary = profile->secondary_addr;
    if (fallback)  *fallback  = profile->fallback;
    return ESP_OK;
}


/* Fan output for boards whose fans hang off an EMC2302 rather than LEDC.
 * Registered with pwm_fan_set_output() once the board has been identified. */
static esp_err_t fan_output_emc2302(int pwm_percent)
{
    if (pwm_percent < 0) {
        pwm_percent = 0;
    } else if (pwm_percent > 100) {
        pwm_percent = 100;
    }

    return EMC2302_set_fan_speed((uint8_t) pwm_percent);
}

esp_err_t init_all_i2c_dev(GlobalState *GLOBAL_STATE)
{
    esp_err_t ret = ESP_FAIL;

    ESP_LOGI(TAG, "Initialize all the i2c dev.");

    /* On the BC01 family nothing downstream of the PD gate is powered until
     * negotiation completes, so this has to run before any attempt to reach
     * the fan controller or the regulator. */
    if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
        if (bc01_pd_bringup(GLOBAL_STATE) != ESP_OK) {
            ESP_LOGE(TAG, "USB-PD bring-up failed; hashboard stays unpowered");
            return ESP_FAIL;
        }

        /* Give the rail time to come up and settle before probing anything
         * behind the gate, then show what is actually reachable. The scan
         * at boot ran before negotiation, so it could only ever have seen
         * the always-on devices. */
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGW(TAG, "Bus after VBUS enabled:");
        bc_i2c_scan();
    }

    if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
        /* No EMC2302 on these boards. The fan is driven directly by LEDC
         * PWM with tach pulse-counting, which is why probing 0x2e here
         * only ever produced a NACK. */
        ESP_ERROR_CHECK(ret = ledc_pwm_init(LEDC_CHANNEL_0));
        ESP_ERROR_CHECK(ret = fan_pcnts_init(LEDC_CHANNEL_0));
        fan_pcnts_clear_counter(LEDC_CHANNEL_0);
        ledc_set_pwm(LEDC_CHANNEL_0, 70);
    } else {
        ESP_ERROR_CHECK(ret = EMC2302_init());
        uint16_t Polarity = nvs_config_get_u16(NVS_CONFIG_INVERT_FAN_POLARITY, 0);
        ret = EMC2302_installed(Polarity);
        if (ESP_OK != ret) {
            return ret;
        }

        /* The automatic thermal controller writes through a hook that
         * defaults to LEDC. On this board there is no LEDC fan, so point it
         * at the controller that is actually fitted -- otherwise the first
         * temperature reading after the ASICs start aborts the miner. */
        pwm_fan_set_output(fan_output_emc2302);
    }

    const device_thermal_profile_t *profile = find_thermal_profile(GLOBAL_STATE->device_model);
    if (NULL == profile) {
        ESP_LOGE(TAG, "DEVICE model %s unknown; supported models are BC01, BC02, BC04, BC06, BC08",
                 GLOBAL_STATE->device_model_str);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = TMP75_init(profile->primary_addr, 0);
    ret = TMP75_installed(0);
    if (ESP_OK != ret) {
        if (NULL != profile->fallback) {
            ESP_LOGE(TAG, "DEVICE model %s identify mismatch, reset model to %s",
                     profile->name, profile->fallback);
            nvs_config_set_string(NVS_CONFIG_DEVICE_MODEL, profile->fallback);
            vTaskDelay(pdMS_TO_TICKS(1000));
            restart_with_reason("Device model identify mismatch");
        }
        ESP_LOGE(TAG, "DEVICE model %s: temperature sensor not responding at 0x%02x",
                 profile->name, profile->primary_addr);
        return ret;
    }

    if (0 != profile->secondary_addr) {
        ret = TMP75_init(profile->secondary_addr, 1);
        ret = TMP75_installed(1);
        if (ESP_OK != ret) {
            ESP_LOGE(TAG, "DEVICE model %s temp2 error", profile->name);
        }
    }

    return ret;
}

/* True when the board carries a second thermal sensor. */
static bool device_has_second_sensor(DeviceModel model)
{
    const device_thermal_profile_t *profile = find_thermal_profile(model);
    return NULL != profile && 0 != profile->secondary_addr;
}

/*
 * Read the hashboard sensors, and say so when a read fails.
 *
 * This used to store whatever TMP75_read_temperature() handed back and return
 * ESP_OK unconditionally. That function reports a failed read as -60 C, so a
 * dead sensor or a dropped I2C bus did not look like a failure to anything
 * upstream -- it looked like a very cold hashboard. The consequences ran the
 * wrong way in both places that use the number: thermal protection compares
 * against 71 C and never tripped, and the fan curve saw -60, decided the board
 * was below its 30 C floor, and dropped the fan to its 18% minimum. A sensor
 * failure therefore made the firmware cool the board *less* while leaving it
 * hashing at full power, with nothing watching the temperature.
 *
 * The reading is still stored on failure, because the web interface wants
 * something to show. The return value is what callers must act on.
 */
esp_err_t read_hash_board_temperature(GlobalState *GLOBAL_STATE)
{
    esp_err_t ret = ESP_OK;
    int8_t value = 0;

    if (ESP_OK == TMP75_get_temperature(0, &value)) {
        GLOBAL_STATE->HEALTH_MODULE.board_temperature[0] = value;
    } else {
        GLOBAL_STATE->HEALTH_MODULE.board_temperature[0] = -60;
        ret = ESP_FAIL;
    }

    if (device_has_second_sensor(GLOBAL_STATE->device_model)) {
        if (ESP_OK == TMP75_get_temperature(1, &value)) {
            GLOBAL_STATE->HEALTH_MODULE.board_temperature[1] = value;
        } else {
            GLOBAL_STATE->HEALTH_MODULE.board_temperature[1] = -60;
            ret = ESP_FAIL;
        }
    }

    return ret;
}

esp_err_t power_on_hashboard(GlobalState *GLOBAL_STATE)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "vcore power on hashboard, set voltage %d", GLOBAL_STATE->asic_vol_default);
    float core_voltage = (float)GLOBAL_STATE->asic_vol_default / 100.0;
    VCORE_set_voltage(core_voltage);

    return ret;
}

esp_err_t power_off_hashboard(GlobalState *GLOBAL_STATE)
{
    /*
     * Report whether the power actually went off.
     *
     * This returned ESP_OK whatever happened, including when the regulator
     * never heard the command. Every caller is a protection path -- overheat,
     * unreadable sensor, fan fault -- so the one thing they all need to know
     * is the one thing this would not tell them: they logged "cut off the
     * power" and waited a hundred seconds for a board that might still be
     * energised.
     *
     * The regulator sits on I2C, so when that bus is gone there is no way to
     * command it. On the BC01 family there is a second lever -- the USB-PD
     * gate is on a different bus, and closing it cuts VBUS to everything
     * behind it -- so it is worth trying when the first attempt fails. The
     * BC04 has no such gate: on that board a failure here is final, and the
     * only remaining action is a person pulling the plug. Saying so in the
     * log is the most this firmware can do for them.
     */
    GLOBAL_STATE->asic_vol_default = 0;
    ESP_LOGI(TAG, "vcore power off hashboard, set voltage 0");

    esp_err_t ret = VCORE_set_voltage(0);
    if (ESP_OK == ret) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "could not command the regulator off (%s)",
             esp_err_to_name(ret));

    if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
        esp_err_t gate = husb238a_gate_close();
        pd_power_io_off();
        if (ESP_OK == gate) {
            ESP_LOGW(TAG, "cut VBUS at the PD gate instead");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "the PD gate did not close either (%s)",
                 esp_err_to_name(gate));
    }

    ESP_LOGE(TAG, "THE HASHBOARD MAY STILL BE POWERED. This board has no way "
                  "to switch it off from software. Remove power by hand.");
    return ret;
}

esp_err_t read_power_information(GlobalState *GLOBAL_STATE)
{
    esp_err_t ret = ESP_OK;

    ret = VCORE_update_power(GLOBAL_STATE);


    return ret;
}

int read_power_temp(void)
{
    return VCORE_get_temp();
}

/*
 * One ceiling, asked for in every place a voltage is set.
 *
 * The vendor caps BC04 and BC08 at 4.80 V and nvs_device.c applied that to
 * the startup default -- but only there. CONFIG_TPS546_VOUT_MAX is 5.20 on a
 * BC04, and both of the other ways in checked against that instead: the boot
 * mode config lifts asic_vol_default to whatever asicnormalvol holds, and the
 * coreVoltage PATCH accepted anything inside min..max and applied it live. So
 * the cap the vendor put on four BM1370s in series could be stepped over by
 * writing one NVS key or sending one request -- 1.30 V a chip instead of
 * 1.20.
 *
 * Whether 1.30 V damages a BM1370 is not something this repository can
 * answer. That the cap existed and did not hold is answerable, and is the
 * defect.
 */
uint16_t device_core_voltage_ceiling(GlobalState * GLOBAL_STATE)
{
    uint16_t ceiling = GLOBAL_STATE->asic_vol_max;

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_BC04:
        case DEVICE_BC08:
            /* Four and eight BM1370 in series; the vendor's figure. */
            if (ceiling > 480) {
                ceiling = 480;
            }
            break;
        default:
            break;
    }

    return ceiling;
}

esp_err_t set_fan_pwm(GlobalState *GLOBAL_STATE, uint8_t pwm_percent)
{
    if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
        return ledc_set_pwm(LEDC_CHANNEL_0, pwm_percent);
    }

    return EMC2302_set_fan_speed(pwm_percent);
}

esp_err_t read_fan_rpm(GlobalState *GLOBAL_STATE)
{
    if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
        int pulses = 0;
        /*
         * fan_pcnts_get_counter() reads the pulse count and then clears it, so
         * the count covers the time since the previous call -- not a fixed
         * window. This passed a hardcoded 1000 ms while the health loop runs
         * every 2000, so every reported RPM was exactly double: 9690 shown
         * against roughly 4500 on stock firmware, on the same fan.
         *
         * Measuring the interval keeps it right if the loop period ever
         * changes, which assuming 2000 would not.
         */
        static int64_t last_us = 0;
        int64_t now_us = esp_timer_get_time();
        uint32_t window_ms = 1000;

        if (last_us != 0) {
            int64_t elapsed_ms = (now_us - last_us) / 1000;
            if (elapsed_ms > 0) {
                window_ms = (uint32_t)elapsed_ms;
            }
        }
        last_us = now_us;

        esp_err_t ret = fan_pcnts_get_rpm(LEDC_CHANNEL_0, &pulses, window_ms);
        GLOBAL_STATE->HEALTH_MODULE.fan_rpm[0] = (uint16_t)pulses;
        GLOBAL_STATE->HEALTH_MODULE.fan_rpm[1] = 0;
        return ret;
    }

    /* Not ESP_ERROR_CHECK: this runs every two seconds in the health loop,
     * and a single I2C hiccup reading a tachometer is not worth aborting the
     * miner for. The caller already treats a bad read as a fan fault. */
    esp_err_t ret = ESP_ERROR_CHECK_WITHOUT_ABORT(
                        EMC2302_get_fan_speed(GLOBAL_STATE->HEALTH_MODULE.fan_rpm));
    GLOBAL_STATE->HEALTH_MODULE.fan_rpm[1] = 0;
    return ret;
}

void reset_hash_board(GlobalState *GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Reset Chain 0.");
    ESP_ERROR_CHECK(reset_pin_init(0));
    reset_pin_low(0);
    volc_delay(500);
    reset_pin_high(0);
    volc_delay(500);
}

void hashboard_reset_pin_init_and_low(GlobalState *GLOBAL_STATE)
{
    ESP_LOGI(TAG, " reset pin init.");
    ESP_ERROR_CHECK(reset_pin_init(0));
    reset_pin_low(0);
    volc_delay(300);
}

void dev_display_init(GlobalState *GLOBAL_STATE)
{
    uint16_t invertScreen = 0;
    if(GLOBAL_STATE->board_version > 1)
    {
        invertScreen = 1;
    }
    invertScreen = nvs_config_get_u16(NVS_CONFIG_FLIP_SCREEN, invertScreen);
    display_s3_init(invertScreen);
}

/*init all the peripherals of esp32 control board, the flow should be passed.*/
esp_err_t init_all_peripherals(GlobalState *GLOBAL_STATE)
{
    esp_err_t ret = ESP_OK;

    start_internal_temperature_sensor();

	init_all_i2c_dev(GLOBAL_STATE);

    //SYSTEM_get_config_by_boot_mode(GLOBAL_STATE);

    /*vcore related sensor init*/
    ESP_ERROR_CHECK(VCORE_init(GLOBAL_STATE));

    /*init the power.*/
    power_on_hashboard(GLOBAL_STATE);
    volc_delay(3000);

    /*reset the hashboard*/
    reset_hash_board(GLOBAL_STATE);

    GLOBAL_STATE->interface_initalized = true;
    return ret;
}

#ifdef STATISTIC_SYSTEM_FEATURE
esp_err_t statistic_init_system_by_device(GlobalState *GLOBAL_STATE)
{
    esp_err_t ret = ESP_OK;

    ret = statistic_init_system(&(GLOBAL_STATE->STATISTIC_MODULE), VOLCMINER_LOTTO_TM_CACULATE_DEFINE);
}
#endif
