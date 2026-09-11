#include "health_maintenance.h"
#include "global_state.h"
#include "system.h"
#include "device.h"

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "vcore.h"
#include "lvgl_porting.h"
#include "influx_task.h"
#include "TPS546.h"
#include "main.h"

const static char * TAG = "health_maintenance";
extern bool b_switch_done[MAX_CHAIN_NUM];

esp_err_t make_fan_input_info(HealthMaintenceModule *health_module, FanInputInfo *fan_input_info)
{
    esp_err_t ret = ESP_FAIL;

    if(NULL == health_module || NULL == fan_input_info){
        ESP_LOGW(TAG, "WARNNING: %p and %p", health_module, fan_input_info);
        goto exit;
    }

    fan_input_info->control_board_temperature = health_module->control_board_temperature;
    if(health_module->board_temperature[0] >=  health_module->board_temperature[1]){
        fan_input_info->highest_board_temperature = health_module->board_temperature[0];
    }else{
        fan_input_info->highest_board_temperature = health_module->board_temperature[1];
    }
    fan_input_info->b_max_fan_pwm = fan_input_info->b_min_fan_pwm = \
        fan_input_info->b_max_fan_pwm_quit = fan_input_info->b_min_fan_pwm_quit = \
            fan_input_info->b_pwm_changed = false;

    for(int i = 0; i < MAX_PWM_CHANNEL; i++){
        fan_input_info->pwm_config[i] = health_module->fan_percent[i];
        fan_input_info->fan_rpm[i] = health_module->fan_rpm[i];
    }
    ret = ESP_OK;

exit:
    return  ret;
}

esp_err_t update_fan_info(HealthMaintenceModule *health_module, FanInputInfo *fan_input_info)
{
    esp_err_t ret = false;

    if(NULL == health_module || NULL == fan_input_info){
        ESP_LOGW(TAG, "WARNNING: %p and %p", health_module, fan_input_info);
        goto exit;
    }

    for(int i = 0; i < MAX_PWM_CHANNEL; i++){
        health_module->fan_percent[i] = fan_input_info->pwm_config[i];
    }
    ret = ESP_OK;

exit:
    return ret;
}

void miner_protection_handler(GlobalState *GLOABAL_STATE)
{
    /*power off and exit*/
    power_off_hashboard(GLOABAL_STATE);
    ESP_LOGW(TAG, "Warning: cut off the power.");

    /*max fan speed.*/
    set_fan_pwm(GLOABAL_STATE, 100);

    /*flag the status*/
    //nvs_config_set_u16(NVS_CONFIG_FAN_SPEED, 100);
    //nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, 0);
    //nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, 1);

    ESP_LOGW(TAG, "Wait for 100 seconds.");
    int i;
    for(i=0;i<10;i++)
    {
        GLOABAL_STATE->SYSTEM_MODULE.current_hashrate = 0;
        read_power_information(GLOABAL_STATE);
        read_hash_board_temperature(GLOABAL_STATE);
        volc_delay(10*1000);
    }
    
    restart_with_reason("Overheat protection triggered");
    exit(ESP_FAIL);
}

void load_health_fan_config(HealthMaintenceModule *healthModule)
{
    uint16_t auto_fan = nvs_config_get_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1);

    if(1 == auto_fan){
        healthModule->fan_eft = false;
    }else if(0 == auto_fan){
        healthModule->fan_eft = true;
        healthModule->fan_percent[0] = healthModule->fan_percent[1] = nvs_config_get_u16(NVS_CONFIG_FAN_SPEED, 100);
    }

    ESP_LOGD(TAG, "load_health_cofig fan %d %"PRIu16" %"PRIu16"", 
            healthModule->fan_eft, healthModule->fan_percent[0], healthModule->fan_percent[1]); 
}

void health_maintenance_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting");
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    HealthMaintenceModule *healthModule = &(GLOBAL_STATE->HEALTH_MODULE);
#ifdef STATISTIC_SYSTEM_FEATURE
    StatisticModule *statistic_module = &(GLOBAL_STATE->STATISTIC_MODULE);
    static uint8_t statistic_counter = 5;
    static bool all_switch_done = false;
#endif
    int64_t time_start, time_elapsed;
    int max_fan_speed = 0, fan_check_param = 0;
    FanInputInfo fan_input_info;
    uint32_t counter = 0, fan_error_counter = 0;
    uint32_t num_of_pwm_channel = 0;
    /*
     * Ten seconds of being driven with no rotation at all, at this loop's two
     * second period. Long enough that a spin-up or a dropped tach sample does
     * not count as a stall.
     */
    #define FAN_STALL_MIN_DUTY 30
    #define FAN_STALL_STRIKES  5
    bool fan_tach_proven[MAX_PWM_CHANNEL] = {false};
    int fan_read_failures = 0;
    static uint8_t lotto_refresh_data_counter = 2;

    led_mutex = xSemaphoreCreateMutex();
    load_health_fan_config(healthModule);

    /*load the health config.*/

    max_fan_speed = 0;
    fan_check_param = MINI_FAN_CHECK_PARAM;
    num_of_pwm_channel = 1;
    int temp_read_failures = 0;

    while(1){
        /*get the infomation from power every 10 seconds.*/
        time_start = esp_timer_get_time() / 1000;

        if(GLOBAL_STATE->interface_initalized){
            if(5 == ++counter){           
                read_power_information(GLOBAL_STATE);
                counter = 0;
                
                #if 0
                static int power_temp = 0;
                power_temp = read_power_temp();

                ESP_LOGI(TAG, "temp: %.1f %d %"PRId8" || %.1fW %.2fV %.1fV", 
                    healthModule->cpu_temperature,
                    power_temp,
                    healthModule->board_temperature[0], 
                    healthModule->power,
                    healthModule->out_voltage, 
                    healthModule->input_voltage
                );
                #endif

                //VCORE_check_fault(GLOBAL_STATE);
            }

            /*
             * A fan we cannot read is a fault to act on, not a reason to
             * abort. ESP_ERROR_CHECK panics, which on a board whose fan
             * controller has gone turns one detectable fault into a reboot
             * loop -- and a panic is the one response that guarantees nobody
             * turns the heat off first. Counted like the temperature read,
             * and it takes the same exit.
             */
            if (ESP_OK != read_fan_rpm(GLOBAL_STATE))
            {
                if (++fan_read_failures >= 3)
                {
                    ESP_LOGE(TAG, "WARNING: fan speed unreadable %d times "
                                  "running -- powering down rather than "
                                  "mining with no idea whether it is turning.",
                             fan_read_failures);
                    SYSTEM_notify_error_info(GLOBAL_STATE, FAN_ERROR, NULL);
                    break;
                }
                ESP_LOGW(TAG, "fan speed read failed (%d/3)", fan_read_failures);
            }
            else
            {
                fan_read_failures = 0;
            }

            ESP_LOGD(TAG, "fan %d %d", healthModule->fan_rpm[0], healthModule->fan_rpm[1]);

            /*get the temperature from sensor.*/
            read_internal_temperature_sensor(&(healthModule->cpu_temperature));
            /*get the temperature from hash board.*/
            if(ESP_OK != read_hash_board_temperature(GLOBAL_STATE))
            {
                /*
                 * No usable temperature. Not the same as a cold board, and
                 * the difference matters: with no reading the overheat test
                 * below cannot fire and the fan curve would drive the fan
                 * down to its minimum, so carrying on means hashing at full
                 * power with nothing watching the heat.
                 *
                 * Three in a row before acting, because a single dropped I2C
                 * transaction is not a dead sensor and this loop runs every
                 * two seconds. Sustained, it takes the same exit as an
                 * overheat: power off, fan to 100%, restart.
                 */
                if(++temp_read_failures >= 3)
                {
                    GLOBAL_STATE->SYSTEM_MODULE.overheat_mode = 1;
                    ESP_LOGE(TAG, "WARNING: hashboard temperature unreadable "
                                  "%d times running -- powering down rather "
                                  "than mining blind.", temp_read_failures);
                    break;
                }
                ESP_LOGW(TAG, "hashboard temperature read failed (%d/3)",
                         temp_read_failures);
            }
            else
            {
                temp_read_failures = 0;
            }

            /*overheat protection.*/
            if(healthModule->control_board_temperature > MAX_ENV_TEMP
                || healthModule->board_temperature[0] > MAX_HASHBOARD_TEMP
                || healthModule->board_temperature[1] > MAX_HASHBOARD_TEMP)
            {
                GLOBAL_STATE->SYSTEM_MODULE.overheat_mode = 1;
                ESP_LOGW(TAG, "WARNNING: enviroment temperature %d, hashborad temperature %d, %d.",
                            healthModule->control_board_temperature,
                            healthModule->board_temperature[0], healthModule->board_temperature[1]
                        );
                break;
            }

            /*
             * Fan stall.
             *
             * What was here could not fire, twice over. check_fan_ok() was
             * passed max_fan_speed = 0 and both of its comparisons are
             * against that value, so it returned true for any RPM including
             * zero; and the trip behind it was gated on force_fan_check,
             * which is declared false and never assigned. Fixing either alone
             * would have changed nothing, which is presumably how it survived
             * so long in a file whose whole job is protection.
             *
             * This asks a narrower question than the curve check did, because
             * a false positive here powers down a working miner: it only
             * looks for a fan that is being driven and is not turning at all.
             * A stopped or disconnected fan reads zero; a slow one does not,
             * and is left to the over-temperature trip, which is the thing
             * that actually matters and is known to work.
             *
             * Only judged once a channel has reported a non-zero speed since
             * boot. A board with no tachometer reads zero forever, and
             * without that latch this would power it off ten seconds in.
             */
            for (uint32_t ch = 0; ch < num_of_pwm_channel; ch++)
            {
                if (healthModule->fan_rpm[ch] > 0)
                {
                    fan_tach_proven[ch] = true;
                }
            }

            /*
             * Latched per channel, not once for the whole board.
             *
             * A shared flag meant one working fan vouched for every other
             * channel: a board reporting Fan0 0 RPM and Fan1 3688 -- which a
             * BC04 on stock firmware does, either because only one fan is
             * fitted or because that tachometer is not wired -- would have had
             * channel 1 prove the tach "works" and then channel 0 read as a
             * stall. That is a shutdown on a healthy miner, which is the one
             * outcome this check must never produce.
             */
            bool fan_stalled = false;
            for (uint32_t ch = 0; ch < num_of_pwm_channel; ch++)
            {
                if (fan_tach_proven[ch] &&
                    healthModule->fan_percent[ch] >= FAN_STALL_MIN_DUTY &&
                    healthModule->fan_rpm[ch] == 0)
                {
                    fan_stalled = true;
                }
            }

            if (fan_stalled)
            {
                if (++fan_error_counter >= FAN_STALL_STRIKES)
                {
                    ESP_LOGE(TAG, "WARNING: fan driven at %"PRIu16"%% and "
                                  "reporting 0 RPM for %"PRIu32" checks -- "
                                  "powering down.",
                             healthModule->fan_percent[0], fan_error_counter);
                    SYSTEM_notify_error_info(GLOBAL_STATE, FAN_ERROR, NULL);
                    break;
                }
                ESP_LOGW(TAG, "fan reporting 0 RPM while driven (%"PRIu32"/%d)",
                         fan_error_counter, FAN_STALL_STRIKES);
            }
            else
            {
                fan_error_counter = 0;
            }

            #if 0
            influx_task_set_temperature(healthModule->board_temperature[0],healthModule->board_temperature[0] + 7);
            float pwr_info[6] = {0.0};
            TPS546_get_pwoer_info(pwr_info);
            influx_task_set_pwr(pwr_info[0], pwr_info[1],
                                pwr_info[2], pwr_info[3],
                                pwr_info[4], pwr_info[5]);
            #endif
            
            /*left road: Fixed pwm.*/
            if(healthModule->fan_eft){
                set_fan_pwm(GLOBAL_STATE, healthModule->fan_percent[0]);
            }else{
                ESP_ERROR_CHECK(make_fan_input_info(healthModule, &fan_input_info));
                lotto_set_pwm_according_to_temperature(&fan_input_info);

                ESP_ERROR_CHECK(update_fan_info(healthModule, &fan_input_info));
            }

            /*update the fan config*/
            load_health_fan_config(healthModule);
        }

        /*led blink.*/
        
        time_elapsed = esp_timer_get_time()/1000 - time_start;
        if(time_elapsed < 2000){
            volc_delay(2000 - time_elapsed);
            

            if(0 == lotto_refresh_data_counter--){
                lotto_refresh_data_counter = 1;
                //refresh_display_data(GLOBAL_STATE);
                refresh_sensor_data_from_system(GLOBAL_STATE);
                refresh_hash_data_from_system(GLOBAL_STATE);
            }


            /*statistic the rt hashrate in 10s*/
            #ifdef STATISTIC_SYSTEM_FEATURE
            if(!all_switch_done && b_switch_done[0] && b_switch_done[1]){
                ESP_LOGI(TAG, "All ths chains is switch-done.");
                all_switch_done = true;
                statistic_clear_average_hash(statistic_module);
            }

            if(0 == statistic_counter--){
                statistic_counter = 5;
                statistic_clear_rt_hash(statistic_module, 10);
            }
            #endif
        }else{
            ESP_LOGW(TAG, "time_elapsed is %"PRIi64"", time_elapsed);
        }
    }

    miner_protection_handler(GLOBAL_STATE);
} 
