#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

#include "self_test.h"
#include "nvs_config.h"
#include "lvgl_porting.h"
#include "vcore.h"
#include "asic.h"
#include "device.h"
#include "hal_i2c.h"
#include "pwm_fan.h"
#include "ip_reporter.h"
#include "network.h"
#include "main.h"
#include "esp_app_desc.h"

static const char *TAG = "self_test";
static GlobalState * GLOBAL_STATE = NULL;

static void tests_done(GlobalState * GLOBAL_STATE, bool test_result) 
{
    GLOBAL_STATE->SELF_TEST_MODULE.result = test_result;
    GLOBAL_STATE->SELF_TEST_MODULE.finished = true;

    /*
     * Record that the test ran whatever the outcome. Writing the flag only on
     * success meant a failing board restarted, tested, failed and restarted
     * again without end -- and because the self test runs before the network
     * comes up, the web interface never appeared, so there was no way in to
     * see the reason or change anything. An unrecoverable loop is a worse
     * failure than a board that boots and reports a fault.
     *
     * 1 is a pass and 2 a recorded failure, so the result survives the restart
     * and can still be reported rather than being silently forgotten.
     */
    if (test_result != true) {
        ESP_LOGW(TAG, "SELF TESTS FAIL !!!");
        nvs_config_set_u16(NVS_CONFIG_SELF_TEST, 2);
    }else{
        ESP_LOGI(TAG, "SELF_TEST OK !!!");
        nvs_config_set_u16(NVS_CONFIG_SELF_TEST,1);
    }
}

/*
 * Ran, but could not: the board has no main supply, so nothing downstream of
 * it can be measured. Recorded as 3 rather than 2 so it is not mistaken for a
 * hardware fault, and so the reason survives the restart and can be reported.
 *
 * Deliberately still recorded rather than left at 0. Leaving it unset would
 * test again on the next boot, and since the self test restarts the miner
 * afterwards, an unpowered board would restart every few seconds -- which is
 * exactly what somebody flashing it over USB does not need. Clearing it is a
 * decision for a person: reflash with the supply connected. There is no API
 * for it -- nothing in the HTTP server reads or writes selftest.
 */
/* Below this the board has no usable main supply: 12 V on a BC04's XT-30, or
 * a negotiated PD contract on the BC01 family. Used both by the gate at the
 * top of the self test and by test_power_on(), so the two cannot disagree
 * about what "powered" means. */
#define SELF_TEST_MIN_VIN 11.0f

void tests_unpowered(GlobalState * GLOBAL_STATE)
{
    GLOBAL_STATE->SELF_TEST_MODULE.result = false;
    GLOBAL_STATE->SELF_TEST_MODULE.active = false;
    ESP_LOGW(TAG, "SELF TEST NOT RUN -- no main power");
    nvs_config_set_u16(NVS_CONFIG_SELF_TEST, 3);
}

esp_err_t test_temperature_sensor(float *temperature)
{
    esp_err_t ret = ESP_FAIL;

    start_internal_temperature_sensor();
    ret = read_internal_temperature_sensor(temperature);
    if(ret == ESP_OK){
        ESP_LOGI(TAG, "Temperature: %.2f °C", *temperature);
    }else{
        ESP_LOGW(TAG, "Failed to read temperature");
    }
    stop_internal_temperature_sensor();

    return ret;
}


/* Board-specific. Hardcoded to 11 in the BC04 tree, but the BC01 puts
 * I2C SDA_1 on GPIO11, so configuring it as a plain input there tore
 * down the bus the core regulator sits on. Comes from Kconfig now. */
#ifndef CONFIG_GPIO_PLUG_SENSE
#define CONFIG_GPIO_PLUG_SENSE      10
#endif
#define GPIO_PLUG_SENSE  CONFIG_GPIO_PLUG_SENSE
#include "TPS546.h"
static TPS546_CONFIG TPS546_CONFIG_LOTTO = {
    /* vin voltage */
    .TPS546_INIT_VIN_ON = 4.8,
    .TPS546_INIT_VIN_OFF = 4.5,
    .TPS546_INIT_VIN_UV_WARN_LIMIT = 0, //Set to 0 to ignore. TI Bug in this register
    .TPS546_INIT_VIN_OV_FAULT_LIMIT = 6.5,
    /* vout voltage */
    .TPS546_INIT_SCALE_LOOP = 0.125,
    .TPS546_INIT_VOUT_MIN = 4,
    .TPS546_INIT_VOUT_MAX = 5,
    .TPS546_INIT_VOUT_COMMAND = 1.2,
    /* iout current */
    .TPS546_INIT_IOUT_OC_WARN_LIMIT = 25.00, /* A */
    .TPS546_INIT_IOUT_OC_FAULT_LIMIT = 30.00 /* A */  
};

esp_err_t test_power(int i2c_master_index)
{
    esp_err_t ret = ESP_FAIL;

    // configure plug sense, if present
    // Configure plug sense pin as input(barrel jack) 1 is plugged in
    gpio_config_t barrel_jack_conf = {
        .pin_bit_mask = (1ULL << GPIO_PLUG_SENSE),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&barrel_jack_conf);
    int barrel_jack_plugged_in = gpio_get_level(GPIO_PLUG_SENSE);
    ESP_LOGI(TAG, "TPS546 power good %d", barrel_jack_plugged_in);

    if(DEVICE_DC04 == GLOBAL_STATE->device_model)
    {
        TPS546_CONFIG_LOTTO.TPS546_INIT_VOUT_MAX = 3.0;
        TPS546_CONFIG_LOTTO.TPS546_INIT_SCALE_LOOP = 0.25;
    }
    else if(DEVICE_DC06 == GLOBAL_STATE->device_model)
    {
        TPS546_CONFIG_LOTTO.TPS546_INIT_VOUT_MAX = 4.5;
        TPS546_CONFIG_LOTTO.TPS546_INIT_SCALE_LOOP = 0.125;
    }
    else if(DEVICE_BC04 == GLOBAL_STATE->device_model)
    {
        TPS546_CONFIG_LOTTO.TPS546_INIT_VOUT_MAX = 5;
        TPS546_CONFIG_LOTTO.TPS546_INIT_SCALE_LOOP = 0.125;
        GLOBAL_STATE->HEALTH_MODULE.voltage = 400;
        GLOBAL_STATE->asic_vol_default = 400;
    }
    else if(DEVICE_BC08 == GLOBAL_STATE->device_model)
    {
        TPS546_CONFIG_LOTTO.TPS546_INIT_VOUT_MAX = 5;
        TPS546_CONFIG_LOTTO.TPS546_INIT_SCALE_LOOP = 0.125;
        GLOBAL_STATE->HEALTH_MODULE.voltage = 400;
        GLOBAL_STATE->asic_vol_default = 400;
    }
    else if(DEVICE_BC06 == GLOBAL_STATE->device_model)
    {
        TPS546_CONFIG_LOTTO.TPS546_INIT_VOUT_MAX = 390;
        TPS546_CONFIG_LOTTO.TPS546_INIT_SCALE_LOOP = 0.125;
        GLOBAL_STATE->HEALTH_MODULE.voltage = 300;
        GLOBAL_STATE->asic_vol_default = 300;
    }
    else if(DEVICE_BC01 == GLOBAL_STATE->device_model)
    {
        /*
         * Single BM1370, so the core domain is one ASIC rather than a
         * series string: roughly 1.2 V, not the 3-4 V of the multi-ASIC
         * boards. The vendor's BC01 self-test source was not released, so
         * these figures come from a running BC01 on 2.0.1 reporting
         * coreVoltage 119 (1.19 V nominal) against 1195 mV measured.
         *
         * VOUT_MAX is deliberately held just above that operating point.
         * Do not raise it to the 5 V the multi-ASIC boards use: on a
         * single-ASIC domain that clamp would permit a voltage far outside
         * the chip's safe range.
         */
        TPS546_CONFIG_LOTTO.TPS546_INIT_VOUT_MAX = 1.5;
        TPS546_CONFIG_LOTTO.TPS546_INIT_SCALE_LOOP = 0.125;
        GLOBAL_STATE->HEALTH_MODULE.voltage = 120;
        GLOBAL_STATE->asic_vol_default = 120;
    }
    else if(DEVICE_BC02 == GLOBAL_STATE->device_model)
    {
        /*
         * Not calibrated. BC02 is a two-ASIC board, so neither the BC01
         * single-ASIC limits nor the BC04 four-ASIC limits apply, and no
         * BC02 hardware was available to measure. Refusing is the safe
         * outcome: running the self-test with the wrong VOUT_MAX risks the
         * ASIC. Normal mining is unaffected -- it takes its voltage from
         * NVS through power_on_hashboard(), not from here.
         */
        ESP_LOGE(TAG, "Self-test is not calibrated for BC02; refusing to set "
                      "PMBus voltage limits. Normal operation is unaffected.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = TPS546_init(TPS546_CONFIG_LOTTO);

    ESP_LOGI(TAG, "test_power done.");
    return ret;      
}

esp_err_t test_power_on(float *vin, float *vout)
{
    esp_err_t ret = ESP_OK;

    power_on_hashboard(GLOBAL_STATE);
    volc_delay(3000);
    *vin = TPS546_get_vin();
    *vout = TPS546_get_vout();
    ESP_LOGI(TAG, "Power in %.2fv, out %.2fv",*vin, *vout);
    if(*vin < SELF_TEST_MIN_VIN)
    {
        ret = ESP_FAIL;
    }
    if(*vout < (GLOBAL_STATE->HEALTH_MODULE.voltage/100 * 0.6) )
    {
        ret = ESP_FAIL;
    }
    return ret;      
}

esp_err_t test_hashboard(void)
{
    esp_err_t ret = ESP_OK;

    /*reset the hashboard*/
    reset_hash_board(GLOBAL_STATE);
    volc_delay(1000);
    GLOBAL_STATE->interface_initalized = true;
    /* Every BC board starts the detect pass at a low frequency. */
    if(DEVICE_BC01 == GLOBAL_STATE->device_model ||
       DEVICE_BC02 == GLOBAL_STATE->device_model ||
       DEVICE_BC04 == GLOBAL_STATE->device_model ||
       DEVICE_BC06 == GLOBAL_STATE->device_model ||
       DEVICE_BC08 == GLOBAL_STATE->device_model)
    {
        GLOBAL_STATE->asic_freqency = 100;
    }
    ret = ASIC_detect(GLOBAL_STATE);
    return ret;      
}

esp_err_t test_external_temperature_sensor(int8_t *t)
{
    esp_err_t ret = ESP_FAIL;

    /*
     * This duplicated device.c's thermal identify logic, once per model,
     * and had no BC01 or BC02 branch -- so those boards fell through and
     * the model was rewritten, which put the device into a reboot loop
     * flipping BC04 -> BC08 -> BC04 and rewriting NVS on every cycle.
     *
     * It now shares the profile table in device.c. A model whose sensor
     * address does not distinguish it from another model is never
     * reconfigured on a missing sensor, because guessing trades a wrong
     * reading for a boot loop.
     */
    uint8_t primary = 0, secondary = 0;
    const char *fallback = NULL;

    if (device_thermal_addresses(GLOBAL_STATE->device_model, &primary, &secondary, &fallback) != ESP_OK) {
        ESP_LOGE(TAG, "No thermal profile for model %s", GLOBAL_STATE->device_model_str);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = TMP75_init(primary, 0);
    ret = TMP75_installed(0);
    if (ESP_OK == ret) {
        *t = TMP75_read_temperature(0);
        ESP_LOGI(TAG, "external_temperature_sensor Temperature: %d C", *t);
    } else if (0 == bc_i2c_devices_found()) {
        /*
         * Same guard as device.c. A silent sensor only means "wrong model"
         * when the bus is alive; with nothing answering at any address every
         * model looks wrong in turn and each adopts the next. A BC04 with a
         * dead hashboard domain ran BC04 -> BC08 -> BC04 indefinitely,
         * rewriting NVS every fourteen seconds, and would leave a repaired
         * board believing it was a different miner.
         *
         * This is the copy that actually fires on a BC04 -- the one in
         * device.c is reached on other paths -- so both need it.
         */
        ESP_LOGE(TAG, "DEVICE model %s: no temperature sensor at 0x%02x, and "
                      "nothing at all answered the bus scan. Keeping the "
                      "stored model -- an empty bus means the hashboard is "
                      "absent, not that this is a different miner.",
                 GLOBAL_STATE->device_model_str, primary);
    } else if (NULL != fallback) {
        ESP_LOGE(TAG, "DEVICE model %s identify mismatch, reset model to %s",
                 GLOBAL_STATE->device_model_str, fallback);
        nvs_config_set_string(NVS_CONFIG_DEVICE_MODEL, fallback);
        vTaskDelay(pdMS_TO_TICKS(1000));
        restart_with_reason("Device model identify mismatch");
    } else {
        ESP_LOGE(TAG, "DEVICE model %s: no temperature sensor at 0x%02x",
                 GLOBAL_STATE->device_model_str, primary);
    }

    return ret;
}


esp_err_t test_fan(uint16_t * rpm)
{
    esp_err_t ret = ESP_FAIL;
    int pwm_conf[] = {0, 50, 80, 100};

    /*
     * The BC01 family has no EMC2302. Its fan is driven straight from LEDC
     * with the tachometer counted in hardware, which device.c has done since
     * bring-up -- but this test still went looking for a controller at 0x2e
     * and failed when nothing answered.
     *
     * That is why a freshly flashed BC01 failed its self test: not a fault on
     * the board, a test written for a different one. The LEDC path has to be
     * started here because the test runs before init_all_i2c_dev() does it.
     */
    bool ledc_fan = device_is_bc01_family(GLOBAL_STATE->device_model);

    if (ledc_fan) {
        ESP_RETURN_ON_ERROR(ledc_pwm_init(LEDC_CHANNEL_0), TAG, "fan pwm");
        ESP_RETURN_ON_ERROR(fan_pcnts_init(LEDC_CHANNEL_0), TAG, "fan tach");
        fan_pcnts_clear_counter(LEDC_CHANNEL_0);
    } else {
        ret = EMC2302_init();
        if (ret != ESP_OK) {
            return ret;
        }
        ret = EMC2302_installed(false);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    for(int i = 0; i < sizeof(pwm_conf)/sizeof(pwm_conf[0]); i++)
    {
        if (ledc_fan) {
            ret = ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_pwm(LEDC_CHANNEL_0, pwm_conf[i]));
            fan_pcnts_clear_counter(LEDC_CHANNEL_0);
            vTaskDelay(pdMS_TO_TICKS(5000));
            /* measures over the window it is given, so it matches the delay.
             * Counted as int, reported as uint16_t like the EMC2302 path. */
            int counted = 0;
            ret = ESP_ERROR_CHECK_WITHOUT_ABORT(
                      fan_pcnts_get_rpm(LEDC_CHANNEL_0, &counted, 5000));
            *rpm = (uint16_t)(counted < 0 ? 0 : counted);
        } else {
            ret = ESP_ERROR_CHECK_WITHOUT_ABORT(EMC2302_set_fan_speed(pwm_conf[i]));
            vTaskDelay(pdMS_TO_TICKS(5000));
            ret = ESP_ERROR_CHECK_WITHOUT_ABORT(EMC2302_get_fan_speed(rpm));
        }
        ESP_LOGI(TAG, "Fan %d%%,speed is %d rpm", pwm_conf[i],*rpm);
        if(pwm_conf[i] < 10)
        {
            if(*rpm > 1000)
            {
                return ESP_FAIL;
            }
        }
        else if(pwm_conf[i] > 90)
        {
            if(*rpm < 1800)
            {
                return ESP_FAIL;
            }
        }
    }
    return ret;
}

esp_err_t test_eth(char * ip)
{
    network_eth_init_test();
	
    int i = 20;
    while(i--)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if(get_eth_ip(ip))
        {
            if(strcmp(ip,"0.0.0.0"))
            {
                return ESP_OK;
            }
        }
    }
    return ESP_FAIL;
}

/*api function */
bool should_test(void)
{
    bool ret = true;
    uint16_t self_test = nvs_config_get_u16(NVS_CONFIG_SELF_TEST, 0);
    
    if(0 == self_test){
        ret = true;                 /* never run */
    }else if(1 == self_test){
        ret = false;                /* passed */
    }else if(2 == self_test){
        /* Ran and failed. Reported, not repeated -- retrying it on every boot
         * is what produced the loop this value exists to break. */
        ESP_LOGW(TAG, "Self test previously failed; not repeating it");
        ret = false;
    }else if(3 == self_test){
        /* Could not be run for lack of main power. Not a fault, and not
         * repeated -- the self test restarts the miner afterwards, so
         * retrying on every boot would leave an unpowered board cycling. */
        ESP_LOGW(TAG, "Self test was skipped: the board had no main power "
                      "when it last ran. Connect the supply and reflash "
                      "to run it.");
        ret = false;
    }else{
        ESP_LOGW(TAG, "NVS ERROR: self_test %"PRIu16"", self_test);
        ret = false;
    }

    return ret;
}


void self_test(GlobalState *global_state)
{
    GLOBAL_STATE = global_state;
    esp_err_t ret = ESP_FAIL;
    char test_str[1000] = "Self Tests : ";
    char test_item_str[64];
    strcat(test_str, GLOBAL_STATE->device_model_str);
    strcat(test_str, "  ");
    strcat(test_str, esp_app_get_description()->version);
    strcat(test_str, "\n\n");
    GLOBAL_STATE->SELF_TEST_MODULE.active = true;
    GLOBAL_STATE->SELF_TEST_MODULE.message = test_str;

    ESP_LOGW(TAG, "Running Self Tests");
    logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);

    float t = 0;
    if((ret = test_temperature_sensor(&t)) != ESP_OK){
        ESP_LOGI(TAG, "Internal Temperature Sensor test failed, %d, %s", ret, esp_err_to_name(ret));
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "interanl temp fail!\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
        tests_done(GLOBAL_STATE, false);
        return;
    }
    else
    {
        sprintf(test_item_str,"internal temp: %0.2f\n",t);
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, test_item_str);
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }
        
    int8_t temp[2] = {0};
    if(ESP_OK != (ret = test_external_temperature_sensor(temp))){
        ESP_LOGE(TAG, "temperature_sensor test failed, %d, %s", ret, esp_err_to_name(ret));
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "temp sensor fail!\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
        tests_done(GLOBAL_STATE, false);
        return;
    }
    else
    {
        if(DEVICE_BC08 == GLOBAL_STATE->device_model || DEVICE_BC06 == GLOBAL_STATE->device_model)
        {
            sprintf(test_item_str,"temp sensor %d, %d\n",temp[0],temp[1]);
        }
        else
        {
            sprintf(test_item_str,"temp sensor %d\n",temp[0]);
        }
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, test_item_str);
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }

    /*
     * Check the supply before judging anything that runs on it.
     *
     * The fan test used to run here with nothing having established that the
     * board has main power at all. test_power_on() does check -- it holds
     * `if (*vin < 11) ret = ESP_FAIL;` -- but it runs afterwards, so on an
     * unpowered board the fan test failed first and the routine returned
     * before the check that would have named the real problem.
     *
     * What that produced, on a BC04 flashed over USB with nothing on the
     * XT-30: a fan turning at 1135 rpm instead of 2693 was recorded as "FAN
     * fail", tests_done() latched it, and should_test() then refused to run
     * again. The board carried a permanent hardware-failure verdict it did
     * not deserve -- confirmed when the owner connected 12 V and it mined on
     * all four ASICs while still reporting the failure.
     *
     * The regulator is not already up here -- the gate brings it up itself,
     * via test_power(). Do not remove that call believing it redundant: the
     * self-test path restarts before init_all_peripherals() ever runs, so
     * nothing else has initialised the TPS546 at this point, and reading VIN
     * without it returns 0.00 V on every board. That mistake has already been
     * made once in this function. Same 11 V threshold as test_power_on(), so
     * the two cannot disagree.
     */
    /*
     * How "no main power" is detected differs by board, because what is
     * reachable differs by board.
     *
     * On the BC01 family the regulator sits behind the USB-PD gate. With no
     * contract it has no power, does not answer I2C at all, and cannot be
     * asked anything -- an attempt to initialise it there just NACKs in a
     * retry loop. So ask the PD layer, which already knows: pd_state is set
     * only when a contract was negotiated and VBUS was enabled.
     *
     * Elsewhere -- a BC04 takes 12 V on an XT-30 -- the regulator runs from
     * the logic rail and answers whether or not the main supply is present.
     * One reported 3.93 V with nothing on its XT-30. There a real measurement
     * is available, so take it, after bringing the regulator up.
     *
     * Not GPIO_PLUG_SENSE, which would be the obvious third option: on a BC04
     * it is left at its default of GPIO10, which is the W5500 interrupt line.
     * It reads something, and that something means nothing.
     */
    float vin_at_start = 0.0f;
    bool have_main_power;
    bool power_inited = false;

    if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
        have_main_power = (1 == GLOBAL_STATE->pd_state);
    } else {
        /*
         * A regulator that does not answer is a fault in its own right, and
         * must not be reported as "no main power". A BC04 with a dead I2C bus
         * would otherwise be told to connect a supply it already has -- the
         * exact failure this gate exists to stop, pointed the other way.
         */
        if (ESP_OK != test_power(I2C_MASTER_INDEX_OF_POWER)) {
            ESP_LOGE(TAG, "The core regulator does not answer. That is a "
                          "fault, not a missing supply.");
            strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "Power init fail!\n");
            logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
            tests_done(GLOBAL_STATE, false);
            return;
        }

        power_inited = true;
        vin_at_start = TPS546_get_vin();
        have_main_power = (vin_at_start >= SELF_TEST_MIN_VIN);
    }

    if (!have_main_power) {
        /* Say which of the two checks failed, and do not quote a voltage
         * that was never measured -- on the BC01 family nothing read the
         * regulator, because it has no power to answer with. */
        if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
            ESP_LOGE(TAG, "No USB-PD contract, so this board has no main "
                          "supply. Not testing the fan or the hashboard, and "
                          "not recording a fault.");
        } else {
            ESP_LOGE(TAG, "Input voltage is %.2f V -- this board has no main "
                          "supply. Not testing the fan or the hashboard, and "
                          "not recording a fault.", vin_at_start);
        }
        ESP_LOGE(TAG, "Connect the main supply and run the self test again.");
        if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
            sprintf(test_item_str, "no main power (no PD)\n");
        } else {
            sprintf(test_item_str, "no main power (%.2fV)\n", vin_at_start);
        }
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, test_item_str);
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
        tests_unpowered(GLOBAL_STATE);
        return;
    }

    uint16_t rpm = 0;
    if((ret = test_fan(&rpm)) != ESP_OK){
        ESP_LOGI(TAG, "Fan test failed, %d, %s", ret, esp_err_to_name(ret));
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "FAN fail!\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
        tests_done(GLOBAL_STATE, false);
        return;
    }
    else
    {
        sprintf(test_item_str,"PWM 100%%,  FAN %d rpm\n",rpm);
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, test_item_str);
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }


    /*
     * Skip when the gate already brought the regulator up. bc_i2c_add_device()
     * has no duplicate check and neither does IDF's
     * i2c_master_bus_add_device(), so calling this twice registers the TPS546
     * on the bus twice: the first handle leaks and a second of the eight
     * device slots is spent. A BC04 uses about five, so it fits today -- and
     * on the day it does not, the add fails, test_power() fails, and every
     * board reports "Power init fail!". Not a risk worth carrying for a call
     * that has already happened.
     */
    if(power_inited){
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "Power init OK\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }
    else if(ESP_OK != (ret = test_power(I2C_MASTER_INDEX_OF_POWER))){
        ESP_LOGE(TAG, "power test failed, %d, %s", ret, esp_err_to_name(ret));
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "Power init fail!\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
        tests_done(GLOBAL_STATE, false);
        return;
    }
    else
    {
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "Power init OK\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }

    float vin,vout;
    if(ESP_OK != (ret = test_power_on(&vin, &vout))){
        ESP_LOGE(TAG, "power on test failed, %d, %s", ret, esp_err_to_name(ret));
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "Power on fail!\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
        tests_done(GLOBAL_STATE, false);
        return;
    }
    else
    {
        sprintf(test_item_str,"Power vin %0.2fV, vout %0.2fV\n",vin,vout);
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, test_item_str);
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }

    if(ESP_OK != (ret = test_hashboard())){
        ESP_LOGE(TAG, "hashboard test failed, %d, %s", ret, esp_err_to_name(ret));
        sprintf(test_item_str,"hashboard test fail! asic=%d\n",GLOBAL_STATE->asic_count[0]);
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, test_item_str);
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
        tests_done(GLOBAL_STATE, false);
        return;
    }
    else
    {
        sprintf(test_item_str,"hashboard OK, asic=%d\n",GLOBAL_STATE->asic_count[0]);
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, test_item_str);
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }

    /*
     * Ethernet last, after the core rail is up -- not before it.
     *
     * This block used to run ahead of test_power_on(), so the W5500 was
     * initialised, linked and holding a DHCP lease at the moment the core
     * voltage stepped up. That is the one failure mechanism this project has
     * direct evidence for: the first BC04 lost its Ethernet controller ~70 ms
     * after that step, and the part later failed short across the 3.3 V rail,
     * taking the I2C domain with it.
     *
     * network_init() defers Ethernet for exactly this reason. The self test
     * went around it, and ran on the first boot after every full flash,
     * because config.<board>.cvs.example ships selftest=0 and a web flash is
     * a full flash. It only ever bit with a cable connected: with none, the
     * test failed and the routine returned before the rail was switched on.
     *
     * Moving it here costs nothing. Nothing above depends on the interface,
     * and a failure is still reported the same way.
     */
    char ip[24];
    if (device_is_bc01_family(GLOBAL_STATE->device_model)) {
        ESP_LOGI(TAG, "No Ethernet on this board; skipping ETH test");
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "ETH n/a\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }
    else if((ret = test_eth(ip)) != ESP_OK){
        ESP_LOGI(TAG, "ETH test failed, %d, %s", ret, esp_err_to_name(ret));
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "ETH fail!\n");
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
        tests_done(GLOBAL_STATE, false);
        return;
    }
    else
    {
        sprintf(test_item_str,"ETH IP %s\n",ip);
        strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, test_item_str);
        logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    }

    strcat(GLOBAL_STATE->SELF_TEST_MODULE.message, "Tests ok!\n");
    logMessage(GLOBAL_STATE->SELF_TEST_MODULE.message);
    tests_done(GLOBAL_STATE, true);
}
