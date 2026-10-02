#ifndef _LVGL_SCREEN_H
#define _LVGL_SCREEN_H

#include <stdint.h>

#include "lvgl.h"

LV_FONT_DECLARE(ui_font_DigitalNumbers16);
LV_FONT_DECLARE(ui_font_DigitalNumbers28);
LV_FONT_DECLARE(ui_font_OpenSansBold13);
LV_FONT_DECLARE(ui_font_OpenSansBold14);
LV_FONT_DECLARE(ui_font_OpenSansBold24);
LV_FONT_DECLARE(ui_font_OpenSansBold45);

#undef REFRESH_UI_ELEMENTS_ONE_BY_ONE
#define REFRESH_UI_ELEMENTS_TOGOTHRE

#define LEGACY_LOAD_SCREEN
#undef ANIMATION_LOAD_SCREEN

/*base on lv_label*/
typedef struct 
{
    size_t screen_index;
    char name[20];
    char text[30];
    char unit_text[5];
    int32_t x_pos;
    int32_t y_pos;
    int32_t width;
    int32_t height;
    lv_align_t align;
    lv_text_align_t text_align;
    lv_color_t text_color;
    const lv_font_t *text_font;
    lv_obj_t *p_lv_obj;
} UiElement;

typedef struct
{
    const lv_image_dsc_t *image_dsc;
    lv_align_t align;
    lv_obj_flag_t add_flag;
    lv_obj_flag_t clear_flag;
    lv_obj_t *p_image_obj;
}ImageElement;

typedef struct {
    float   input_voltage_v;
    float   output_voltage_mv;
    float   output_current_ma;
    float   output_power_w;
    float   efficiency_w_per_m;

    uint16_t   temperature;
    int64_t uptime_seconds;
    double hashrate;

    char    str_ip[20];
    char    str_best_diff[20];
    uint16_t rpm;
    char    str_asic_model[20]; /*device model.*/
    int8_t     rssi;
    char    model_str[10];
}mining_screen_data;

typedef struct {
    lv_obj_t            *m_screen_obj;
    mining_screen_data  m_mining_screen_data;
    ImageElement        m_image_element;
    ImageElement        m_image_element_wifi;
    UiElement           m_ui_element_array[13];
}mining_screen;

typedef struct {
    char ssid[20];
}portal_screen_data;

typedef struct {
    lv_obj_t            *m_screen_obj;
    portal_screen_data  m_portal_screen_data;
    ImageElement        m_image_element;
    UiElement           m_ui_element_array[1];
    lv_obj_t            *m_qr_wifi_obj;
    lv_obj_t            *m_qr_wifi;
}portal_screen;

typedef struct {
    double hashrate;
    int8_t temperature;
    char ltc_price[20];
    uint16_t block_num;
}doge_screen_data;

/*
 * One jar, one fact at a time. The facts live in the refresh function rather
 * than here because nothing outside the display ever sets them -- unlike every
 * other screen on this device, this one has no data coming in.
 */
typedef struct {
    lv_obj_t            *m_screen_obj;
    ImageElement        m_image_element;
    UiElement           m_ui_element_array[1];
}bathwater_screen;

typedef struct {
    lv_obj_t            *m_screen_obj;
    doge_screen_data    m_doge_screen_data;
    ImageElement        m_image_element;
    UiElement           m_ui_element_array[4];
}doge_screen;

typedef struct {
    char global_diff[20];
    char global_hashrate[20];
    char ltc_blockheight[20];

    char halving_progress[20];
    char halving_blocks[20];

    /* price is shown large on this page now, so it is carried here as well
     * as on the coin screen */
    char price[20];
}global_stats_data;

typedef struct {
    lv_obj_t            *m_screen_obj;    
    global_stats_data   m_global_stats_data;
    ImageElement        m_image_element;
    UiElement           m_ui_element_array[3];
}global_stats_screen;

typedef struct {
    char dummy_string[20];
}init2_screen_data;

typedef struct {
    lv_obj_t            *m_screen_obj;    
    init2_screen_data   m_init2_data;
    ImageElement        m_image_element;
}init2_screen;

typedef struct{
    float   output_voltage_mv;
    uint16_t   frequency;
    char    fan_mode[20];

    char    str_best_diff[20];
    double  hashrate;

    char str_ip[20];

    char    pool_url[30];
    uint16_t pool_port;
    uint32_t total_shares;
    float   efficency;
    int8_t     rssi;
}setting_data;

typedef struct{
    lv_obj_t            *m_screen_obj;
    setting_data        m_setting_data;
    ImageElement        m_image_element;
    ImageElement        m_image_element_wifi;
    UiElement           m_ui_element_array[10];
}setting_screen;

typedef struct{
    char note[20];
}splash_data;

typedef struct{
    lv_obj_t            *m_screen_obj;
    splash_data         m_splash_data;
    ImageElement        m_image_element;
    UiElement           m_ui_element_array[2];
}splash_screen;

typedef struct{
    lv_obj_t            *m_screen_obj;
    ImageElement        m_image_element;
    UiElement           m_ui_element_array[1];
}log_screen;

/* Local time, with the miner's own hashrate kept small in the corner. */
typedef struct{
    char clock_time[20];
    char clock_date[20];
    char hashrate[20];
    /* "A 50% / B 50%" while dual mining, empty otherwise */
    char pool_split[20];
}clock_screen_data;

typedef struct{
    lv_obj_t            *m_screen_obj;
    clock_screen_data   m_clock_data;
    ImageElement        m_image_element;
    UiElement           m_ui_element_array[4];
}clock_screen;

typedef struct {
    /*SemaphoreHandle_t xMutex;*/
    init2_screen    m_init2_screen;
    portal_screen   m_portal_screen;
    mining_screen m_mining_screen;
    setting_screen m_setting_screen;
    doge_screen   m_doge_screen;
    bathwater_screen m_bathwater_screen;
    global_stats_screen  m_global_screen;
    splash_screen    m_splash_screen;
    log_screen m_log_screen;
    clock_screen m_clock_screen;

    /* pushed in by refresh_hash_data_from_system, so the display module keeps
     * its habit of never reaching into GlobalState itself */
    bool m_dual_enable;
    uint8_t m_dual_ratio_a;
}volc_display;

typedef struct{
    float   input_voltage_v;
    float   output_voltage_mv;
    float   output_current_ma;
    float   output_power_w;

    int8_t temperature;
    uint16_t rpm;
    char fan_mode[20];
    float   efficency;
}sensor_data;

typedef struct{
    double hashrate;
    uint16_t frequency;
    uint64_t total_shares;
    uint64_t shares_rejected;
    int64_t uptime_seconds;
    char str_best_diff[20];
    uint16_t block_num;
}hash_data;

typedef struct{
    char ltc_price[20], doge_price[20];
    char ltc_total_hashrate[20], doge_total_hashrate[20];
    uint32_t ltc_block_height, doge_block_height;
    char global_ltc_diff[20], global_doge_diff[20];
    char halving_progress[20], halving_blocks[20];
}coin_info;

typedef struct{
    char ssid[20];
    char str_ip[20];
    char pool_url[30];
    uint16_t pool_port;
}network_setting;

typedef enum {
    NEXT_KEY_PREESSED = 0,
    DISPLAY_KEY_PREESSED,
    BLOCK_FOUND,
    OVERHEATE_WARNING,
    WIFI_STATE_CONNECTED,
    UPDATE_MINING_STATUS,
    UPDATE_SETTINGS,
    UPDATE_PRICE,
}SCREEN_MSG_TYPE;

typedef struct {
    SCREEN_MSG_TYPE msg_type;
    char update_msg[20];  
}screen_msg;

typedef enum {
    SCREEN_INIT_SCREEN = 0,
    SCREEN_PORTAL_SCREEN,
    SCREEN_SPLASH_SCREEN,
    SCREEN_GLOBAL_SCREEN,
    SCREEN_DOGE_SCREEN,
    SCREEN_BATHWATER_SCREEN,
    SCREEN_MINING_SCREEN,
    SCREEN_SETTING_SCREEN,
    SCREEN_CLOCK_SCREEN,
    SCREEN_LOG_SCREEN,
    SCREEN_NUM
}SCREEN_INDEX;

typedef enum {
    STATS_BOOT_UP = 0,
    STATS_WIFI_CONNECTING,
    STATS_INIT,
    STATS_MINING
}SCREEN_STATS;

void init_all_screen(lv_disp_t *disp);
void show_first_screen();

void display_backlight_on();
void display_backlight_off();
void display_backlight_invert();

void lv_update_screen_cb();
void refresh_current_screen();
void logMessage(const char *message);

void showFoundBlockScreen(void);
void hideFoundBlockScreen(void);
void showErrorScreen(const char *error_message, uint32_t error_code);
void hideErrorScreen(void);

void refresh_coin_data(coin_info new_coin_info);
void refresh_clock_screen(bool b_load_screen);
void refresh_bathwater_screen(bool b_load_screen);

#endif