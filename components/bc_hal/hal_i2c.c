#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"

#include "miner.h"
#include "hal_i2c.h"
#include "HUSB238A.h"
#include "TPS546.h"
#include "pmbus_commands.h"

static const char * TAG = "hammer-i2c";

static i2c_master_bus_handle_t i2c_bus_handle[2];
static i2c_dev_map_entry_t i2c_device_map[MAX_DEVICES];
static int i2c_device_count = 0;

static esp_err_t log_on_error(esp_err_t err, i2c_master_dev_handle_t handle) 
{
    if (err == ESP_OK) 
	{
        return ESP_OK;
    }

    for (int i = 0; i < i2c_device_count; i++) 
	{
        if (i2c_device_map[i].handle == handle) 
		{
            ESP_LOGE(TAG, "Device %s (0x%02x)", i2c_device_map[i].device_tag, i2c_device_map[i].device_address);
            return err;
        }
    }
    
    ESP_LOGE(TAG, "Unknown device");
    return err;
}

/*
 * Find pins that carry an external pull-up, without driving anything.
 *
 * The BC boards do not all put their sensors on one bus. This
 * configuration inherits the BC04's single bus on 43/44 and leaves bus 1
 * disabled, so a device on a second bus is invisible no matter how the
 * scan is run. An I2C line always has a pull-up on it; a floating pin does
 * not.
 *
 * Reading each candidate twice, once with the internal pull-up and once
 * with the internal pull-down, separates them: a pin held high against our
 * own pull-down has something external holding it up. This only ever
 * configures inputs, so it cannot contend with an output on the carrier.
 */
/*
 * Look for solder bridges between the pins that reach the hashboard.
 *
 * Written for hand-soldered headers on a replacement T-Display module, where
 * a bridge between two adjacent pins is the most likely mistake and the one
 * hardest to see. It needs no hashboard attached -- in fact it is cleaner
 * without one, because the only thing that should pull a pin low is another
 * pin it is bridged to.
 *
 * Each pin in turn is driven low while every other pin sits input-with-pull-up.
 * A pin that follows the driven one down shares a net with it. On a correctly
 * soldered module nothing follows anything.
 *
 * What it cannot find is an open joint. Nothing here reaches past the header,
 * so a pin soldered to nothing looks exactly like a pin soldered correctly to
 * a hashboard that is not answering. Only hardware that would otherwise reply
 * can prove continuity.
 *
 * GPIO14 is read but never driven. It is the NEXT button, and driving it means
 * gpio_config(), which clears the negative-edge interrupt dev_display_init()
 * registered on it -- see the note further up this file, where exactly that
 * silently stopped the button changing pages. Reading a pin someone else has
 * configured is free, so a bridge onto 14 still shows up when its neighbour is
 * the one being driven.
 *
 * Only called where the bus scan found nothing, so there is no powered
 * hashboard driving these lines against us.
 */
void hammer_gpio_bridge_test(void)
{
    /* Same lines as the survey, minus GPIO14: driven, not merely read. */
    static const int drivers[] = { 2, 3, 10, 11, 12, 13, 16, 21, 43, 44 };
    /* Everything worth watching, GPIO14 included. */
    static const int watched[] = { 2, 3, 10, 11, 12, 13, 14, 16, 21, 43, 44 };

    const size_t n_drv = sizeof(drivers) / sizeof(drivers[0]);
    const size_t n_wat = sizeof(watched) / sizeof(watched[0]);
    int bridges = 0;

    ESP_LOGI(TAG, "GPIO bridge test (each pin driven low in turn; nothing "
                  "should follow it)");

    /* Every driveable line idle, input with a pull-up, so only a bridge can
     * take one down. */
    for (size_t i = 0; i < n_drv; i++) {
        gpio_config_t up = {
            .pin_bit_mask = (1ULL << drivers[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&up);
    }
    esp_rom_delay_us(5000);

    for (size_t i = 0; i < n_drv; i++) {
        int drive = drivers[i];

        gpio_config_t out = {
            .pin_bit_mask = (1ULL << drive),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&out);
        gpio_set_level(drive, 0);
        esp_rom_delay_us(3000);

        for (size_t j = 0; j < n_wat; j++) {
            int watch = watched[j];
            if (watch == drive) {
                continue;
            }
            if (0 == gpio_get_level(watch)) {
                ESP_LOGE(TAG, "  BRIDGE: GPIO%d follows GPIO%d low -- these "
                              "two share a net", watch, drive);
                bridges++;
            }
        }

        /* Back to idle before the next one is driven. */
        gpio_config_t up = {
            .pin_bit_mask = (1ULL << drive),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&up);
        esp_rom_delay_us(3000);
    }

    /* Leave them as the survey does: floating, claiming nothing. */
    for (size_t i = 0; i < n_drv; i++) {
        gpio_config_t off = {
            .pin_bit_mask = (1ULL << drivers[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&off);
    }

    if (0 == bridges) {
        ESP_LOGI(TAG, "  no bridges: every pin stayed high while its "
                      "neighbours were pulled down");
    } else {
        ESP_LOGE(TAG, "  %d bridged pin pair(s) -- each is reported twice, "
                      "once from each end", bridges);
    }
    ESP_LOGW(TAG, "  this finds shorts, not opens. A pin soldered to nothing "
                  "reads the same as a good one.");
}

void hammer_gpio_pullup_survey(void)
{
    /* Every GPIO broken out on the module that this firmware does not
     * already claim for the display, UART or bus 0. */
    static const int candidates[] = { 2, 3, 10, 11, 12, 13, 14, 16, 21, 43, 44 };

    ESP_LOGI(TAG, "GPIO pull-up survey (external pull-up implies a bus line)");

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int pin = candidates[i];
        gpio_config_t up = {
            .pin_bit_mask = (1ULL << pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&up);
        esp_rom_delay_us(2000);
        int with_pullup = gpio_get_level(pin);

        gpio_config_t down = {
            .pin_bit_mask = (1ULL << pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
        };
        gpio_config(&down);
        esp_rom_delay_us(2000);
        int with_pulldown = gpio_get_level(pin);

        /* Leave the pin floating again. */
        gpio_config_t off = {
            .pin_bit_mask = (1ULL << pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&off);

        const char *verdict = "floating";
        if (with_pullup && with_pulldown) {
            verdict = "EXTERNAL PULL-UP  <-- candidate bus line";
        } else if (!with_pullup && !with_pulldown) {
            verdict = "driven low externally";
        }

        ESP_LOGW(TAG, "  GPIO%-2d  pu=%d pd=%d  %s", pin, with_pullup, with_pulldown, verdict);
    }
}

/*
 * Measure a pin's resting voltage with the internal pull-up engaged.
 *
 * The digital survey can tell that something holds a pin low, but not
 * what. A pull-down resistor forms a divider against the chip's ~45k
 * internal pull-up and settles at a few hundred millivolts; an actively
 * driven output holds near zero. That distinction decides whether the pin
 * is an input we may drive or an output we must not fight, so it is worth
 * measuring rather than assuming. ADC1 covers GPIO1..GPIO10.
 */
void hammer_gpio_measure(int pin)
{
    if (pin < 1 || pin > 10) {
        ESP_LOGW(TAG, "  GPIO%d is outside ADC1, cannot measure", pin);
        return;
    }

    gpio_config_t up = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&up);
    esp_rom_delay_us(5000);

    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit_cfg, &adc) != ESP_OK) {
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_channel_t channel = (adc_channel_t)(pin - 1);   /* GPIO1 -> CH0 */
    if (adc_oneshot_config_channel(adc, channel, &chan_cfg) == ESP_OK) {
        int raw = 0, mv = 0;
        adc_cali_handle_t cali = NULL;
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        bool have_cali = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali) == ESP_OK);

        if (adc_oneshot_read(adc, channel, &raw) == ESP_OK) {
            if (have_cali) {
                adc_cali_raw_to_voltage(cali, raw, &mv);
            }
            ESP_LOGW(TAG, "  GPIO%d with internal pull-up: raw=%d  %d mV  -> %s",
                     pin, raw, mv,
                     mv > 250 ? "resistor pull-down (an input; safe to drive)"
                              : "held near ground (likely a driven output; do not drive)");
        }
        if (have_cali) {
            adc_cali_delete_scheme_curve_fitting(cali);
        }
    }
    adc_oneshot_del_unit(adc);

    gpio_config_t off = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&off);
}

esp_err_t bc_i2c_init(void)
{
    /*
     * hammer_gpio_pullup_survey() and hammer_gpio_measure() used to run here.
     * They must not: both call gpio_config(), which resets a pin's interrupt
     * type to GPIO_INTR_DISABLE unless one is given. dev_display_init() has
     * already registered the NEXT button's negative-edge interrupt on GPIO14
     * by this point, and GPIO14 is in the survey's candidate list -- so the
     * survey silently disabled it and the button stopped changing pages.
     * GPIO0, the other button, is not in the list, which is why that one kept
     * working and made the fault look like a wiring problem.
     *
     * Both are kept for bringing up an unfamiliar board, but they mutate pins
     * other subsystems own and have no business running on every boot.
     */

    /*
     * Two candidate explanations for the missing regulator were tested here
     * on a BC01 and both were disproved, so neither is left in the code:
     *
     *   - Driving GPIO10 high, on the theory that its ~7.5k pull-down made
     *     it a hashboard enable defaulted off. No effect on the bus.
     *   - Releasing the chain 0 reset on GPIO1 before scanning, in case
     *     hashboard devices were held in reset. No effect either.
     *
     * See docs/BC01-BRINGUP.md for the full set of measurements.
     */

    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = GPIO_I2C_SCL_0,
        .sda_io_num = GPIO_I2C_SDA_0,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle[0]));

    /*
     * The BC01 splits its devices across two buses. The thermal sensor and
     * the USB-PD controller sit on bus 0, but the TPS546 core regulator is
     * on bus 1, behind the PD gate -- so its pull-ups are unpowered until
     * negotiation completes, and it is invisible before then. Boards that
     * leave GPIO_I2C_SDA_1 at 255 simply do not get a second bus.
     */
    if (GPIO_I2C_SDA_1 != 255 && GPIO_I2C_SCL_1 != 255) {
        i2c_master_bus_config_t i2c_bus_config_1 = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = I2C_NUM_1,
            .scl_io_num = GPIO_I2C_SCL_1,
            .sda_io_num = GPIO_I2C_SDA_1,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config_1, &i2c_bus_handle[1]));
        ESP_LOGI(TAG, "I2C bus 1 on SDA=%d SCL=%d", GPIO_I2C_SDA_1, GPIO_I2C_SCL_1);
    } else {
        i2c_bus_handle[1] = NULL;
    }
    
    //wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    bc_i2c_scan();

    return ESP_OK;
}

/*
 * Read a few identifying registers from a device found by the scan.
 *
 * The BC boards do not agree on where their sensors and regulators sit,
 * and the vendor released headers for only some models, so knowing an
 * address is not the same as knowing what answers there. PMBus parts
 * respond to 0x98/0x99/0xAD; a TMP75 has no such registers and simply
 * echoes its pointer register, which is itself a useful signature.
 */
static void bc_i2c_identify(uint8_t addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_BUS_SPEED_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;

    if (i2c_master_bus_add_device(i2c_bus_handle[0], &dev_cfg, &dev) != ESP_OK) {
        return;
    }

    static const struct { uint8_t reg; const char *name; uint8_t len; } probes[] = {
        { PMBUS_REVISION,        "PMBUS_REVISION",  1 },
        { PMBUS_MFR_ID,          "MFR_ID",          6 },
        { PMBUS_MFR_MODEL,       "MFR_MODEL",       6 },
        { PMBUS_IC_DEVICE_ID,    "IC_DEVICE_ID",    6 },
        { PMBUS_STATUS_WORD,     "STATUS_WORD",     2 },
        { PMBUS_READ_VIN,        "READ_VIN",        2 },
        { PMBUS_READ_VOUT,       "READ_VOUT",       2 },
        { PMBUS_READ_TEMPERATURE_1, "READ_TEMP_1",  2 },
        { 0x00,                  "reg0x00",         2 },
        { 0x01,                  "reg0x01",         2 },
        { 0x02,                  "reg0x02",         2 },
        { 0x03,                  "reg0x03",         2 },
        { 0x04,                  "reg0x04",         2 },
        { 0x05,                  "reg0x05",         2 },
        { 0x06,                  "reg0x06",         2 },
        { 0x07,                  "reg0x07",         2 },
        { 0x08,                  "reg0x08",         2 },
        /* INA226/228 identity: MFR_ID reads 0x5449 ("TI"), DIE_ID 0x2260. */
        { 0xFE,                  "MFR_ID_FE",       2 },
        { 0xFF,                  "DIE_ID_FF",       2 },
    };

    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint8_t out[8] = {0};
        uint8_t reg = probes[i].reg;
        if (i2c_master_transmit_receive(dev, &reg, 1, out, probes[i].len, 100) == ESP_OK) {
            char hex[32] = {0};
            for (uint8_t b = 0; b < probes[i].len && b < 8; b++) {
                snprintf(hex + b * 3, sizeof(hex) - b * 3, "%02x ", out[b]);
            }
            ESP_LOGD(TAG, "    0x%02x %-14s = %s", addr, probes[i].name, hex);
        }
    }

    i2c_master_bus_rm_device(dev);
}

/*
 * Log every address that acknowledges on the bus.
 *
 * Sensor addresses differ between BC models and the vendor released the
 * headers for only some of them, so a board whose sensor is not where the
 * table expects looks identical to a board with no sensor at all. This
 * turns that into an answer rather than a guess, and it costs one pass at
 * boot.
 */
/*
 * How many devices answered the last bus scan, or -1 before one has run.
 *
 * Kept because "the bus is empty" and "this device is not at that address"
 * lead to opposite conclusions, and only the scan can tell them apart.
 */
static int s_devices_found = -1;

int bc_i2c_devices_found(void)
{
    return s_devices_found;
}

void bc_i2c_scan(void)
{
    int found = 0;

    /*
     * A NACK is this function's answer, not a fault: it is how a bus reports
     * that nothing lives at an address, and how a chip reports that it has no
     * such register. The scan asks both questions deliberately -- a TMP75 has
     * four registers and gets probed for nineteen -- and the IDF driver logs
     * every one at ERROR level. A healthy board therefore produced twenty-two
     * red lines at each boot that read as failing hardware.
     *
     * Silence the driver for the duration and restore whatever level was set,
     * so real I2C faults outside the scan still report normally.
     */
    esp_log_level_t prev = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    ESP_LOGI(TAG, "Scanning I2C bus (SDA=%d SCL=%d)", GPIO_I2C_SDA_0, GPIO_I2C_SCL_0);

    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(i2c_bus_handle[0], addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  device responding at 0x%02x", addr);
            bc_i2c_identify(addr);
            found++;
        }
    }

    esp_log_level_set("i2c.master", prev);

    s_devices_found = (int)found;

    if (found == 0) {
        ESP_LOGE(TAG, "  no devices found -- check hashboard power and wiring");
        /*
         * An empty bus has two very different causes and the scan cannot tell
         * them apart: the devices are unpowered, or they are powered and not
         * answering. The pull-ups separate them, because they are fed from the
         * same rail as the devices.
         *
         *   SDA/SCL show EXTERNAL PULL-UP -> the rail is up and the bus is
         *                                    intact; the devices themselves
         *                                    are dead or absent
         *   SDA/SCL floating              -> no pull-ups, so no rail: the
         *                                    hashboard supply is missing
         *   SDA/SCL driven low            -> the bus is shorted down
         *
         * Only worth the two seconds when there is nothing on the bus at all,
         * which is why it does not run on a healthy boot.
         */
        ESP_LOGW(TAG, "  surveying the bus pins to tell 'unpowered' from 'dead'");
        hammer_gpio_pullup_survey();
        hammer_gpio_bridge_test();
    } else {
        ESP_LOGI(TAG, "  %d device(s) found", found);
    }
}

esp_err_t bc_i2c_add_device(uint8_t device_address, 
        i2c_master_dev_handle_t * dev_handle, const char *device_tag)
{
    if (i2c_device_count >= MAX_DEVICES) 
	{
        ESP_LOGE(TAG, "Device map full, cannot add more devices");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = 
	{
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_address,
        .scl_speed_hz = I2C_BUS_SPEED_HZ,
    };

    /* The core regulator lives on bus 1 where that bus exists. */
    i2c_master_bus_handle_t target = i2c_bus_handle[0];
    if (device_address == TPS546_I2CADDR && i2c_bus_handle[1] != NULL) {
        target = i2c_bus_handle[1];
    }
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(target, &dev_cfg, dev_handle), TAG, "Device 0x%02x", device_address);

    i2c_device_map[i2c_device_count].handle = *dev_handle;
    i2c_device_map[i2c_device_count].device_address = device_address;
    strncpy(i2c_device_map[i2c_device_count].device_tag, device_tag, sizeof(i2c_device_map[i2c_device_count].device_tag) - 1);
    i2c_device_map[i2c_device_count].device_tag[sizeof(i2c_device_map[i2c_device_count].device_tag) - 1] = '\0';
    i2c_device_count++;

    return ESP_OK;
}

esp_err_t bc_i2c_get_bus_handle(i2c_master_bus_handle_t * dev_handle)
{
    *dev_handle = i2c_bus_handle[0];
    return ESP_OK;
}

esp_err_t bc_i2c_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t * read_buf, size_t len)
{
    return log_on_error(i2c_master_transmit_receive(dev_handle, &reg_addr, 1, read_buf, len, I2C_DEFAULT_TIMEOUT), dev_handle);
}

esp_err_t bc_i2c_register_write_addr(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr)
{
    return log_on_error(i2c_master_transmit(dev_handle, &reg_addr, 1, I2C_DEFAULT_TIMEOUT), dev_handle);
}

esp_err_t bc_i2c_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};

    return log_on_error(i2c_master_transmit(dev_handle, write_buf, 2, I2C_DEFAULT_TIMEOUT), dev_handle);
}

esp_err_t bc_i2c_register_write_word(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint16_t data)
{
    uint8_t write_buf[3] = {reg_addr, (uint8_t)(data & 0x00FF), (uint8_t)((data & 0xFF00) >> 8)};

    return log_on_error(i2c_master_transmit(dev_handle, write_buf, 3, I2C_DEFAULT_TIMEOUT), dev_handle);
}

esp_err_t bc_i2c_register_write_bytes(i2c_master_dev_handle_t dev_handle, uint8_t * data, uint8_t len)
{
    return log_on_error(i2c_master_transmit(dev_handle, data, len, I2C_DEFAULT_TIMEOUT), dev_handle);
}
