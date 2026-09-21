#ifndef _HEALTH_MAINTENNANCE_H
#define _HEALTH_MAINTENNANCE_H

#include "stdint.h"

#include "miner.h"

typedef struct {
    float  cpu_temperature;
    int8_t control_board_temperature;
    int8_t board_temperature[MAX_CHAIN_NUM];

    bool fan_eft;
    uint16_t fan_percent[MAX_PWM_CHANNEL];
    /*
     * fan_rpm[0] is the effective reading -- the higher of the two tachometer
     * channels -- and every existing consumer (the display, the stall check,
     * the fanrpm API field) reads it. fan_rpm[1] is left at zero.
     *
     * fan_rpm_raw[] is what each channel actually reported. These boards have
     * two fan headers and ship with one fan fitted, so on a healthy miner one
     * of these is zero, and which one is not fixed across boards. Reported
     * for diagnosis only: nothing acts on it, because an unpopulated header
     * and a stalled fan produce the same reading.
     */
    uint16_t fan_rpm[MAX_PWM_CHANNEL];
    uint16_t fan_rpm_raw[MAX_PWM_CHANNEL];

    float voltage;
    float input_voltage;
    float out_voltage;
    float out_voltage1;
    float out_current;
    float power;
    float max_power;
    int nominal_input_voltage;
}HealthMaintenceModule;

void health_maintenance_task(void *pvParameters);
void health_maintenance_task_lotto(void *pvParameters);
#endif