#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <string.h>

#include "lvgl_screen.h"
#include "lvgl_porting.h"
#include "lilygo_porting.h"
#include "system.h"
#include "ip_reporter.h"
#include "nvs_config.h"

#include "themes.h"
#include "network.h"
#include "main.h"

static const char *TAG = "lvgl_screen";

esp_err_t get_wifi_current_rssi(int8_t *rssi);

QueueHandle_t screen_msg_queue = NULL;
static SCREEN_INDEX cur_screen_index = SCREEN_INIT_SCREEN;
static SCREEN_STATS screen_status = STATS_BOOT_UP;

static bool b_screen_changed = false;
static bool display_on_off = false;

static lv_style_t style;
static volc_display volc_s3_display = {
     .m_init2_screen = {
        .m_screen_obj = NULL,
        .m_init2_data = {.dummy_string = "\0"},
        .m_image_element = {.image_dsc = &initscreen2, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE}
    },
    .m_portal_screen = {
        .m_screen_obj = NULL,
        .m_portal_screen_data = {.ssid = "myssid"},
        .m_image_element = {.image_dsc = &portalscreen, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE},
        .m_ui_element_array = {
            {.name="ssid", .text="myssid", .x_pos=50, .y_pos=63, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT,.height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0xff, 0xff, 0xff)}
        }
    },
    .m_mining_screen = {
        .m_screen_obj = NULL,
        .m_mining_screen_data = {.input_voltage_v = 12.10, .output_voltage_mv = 1210, .output_current_ma = 10, .output_power_w = 20.0, .efficiency_w_per_m = 200.0,
            .temperature = 20.0,.uptime_seconds = 0,.hashrate = 100,.str_ip = "0.0.0.0",.str_best_diff = "0M", .rpm = 2000, .str_asic_model = "BM1370",.rssi = -50},
        .m_image_element = {.image_dsc = &miningscreen2, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE}, 
        .m_image_element_wifi = {.image_dsc = &wifi1, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE}, 
        .m_ui_element_array = {
            {.name="input_voltage_v", .text="12.0V", .unit_text="V", .x_pos=86, .y_pos=-36, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="output_voltage_mv", .text="1190mv", .unit_text="mV", .x_pos=86, .y_pos=-12, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
             .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="output_current_ma", .text="2.33mA", .unit_text="mA", .x_pos=86, .y_pos=10, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="power_w", .text="0W", .unit_text="W", .x_pos=86, .y_pos=34, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="eficiency", .text="230", .unit_text="", .x_pos=-200, .y_pos=62, .text_font=&lv_font_montserrat_16,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="temperature", .text="48", .unit_text="", .x_pos=-282, .y_pos=0, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="up_time", .text="1d 2h 5m", .unit_text="", .x_pos=-16, .y_pos=8, .text_font=&lv_font_montserrat_12,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="ip", .text="192.168.1.200", .unit_text="", .x_pos=80, .y_pos=-75, .text_font=&lv_font_montserrat_10,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="best diff", .text="22M", .unit_text="", .x_pos=240, .y_pos=28, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="hashrate", .text="500,0", .unit_text="", .x_pos=-48, .y_pos=63, .text_font=&lv_font_montserrat_28,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="rpm", .text="5000", .unit_text="", .x_pos=-130, .y_pos=-68, .text_font=&lv_font_montserrat_12,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="asic_module", .text="1760", .unit_text="", .x_pos=-50, .y_pos=-66, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="model_str", .text="BC04", .unit_text="", .x_pos=112, .y_pos=-20, .text_font=&lv_font_montserrat_20,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0xE1, 0xFF, 0x3C)}
        }
    },
    .m_setting_screen = {
        .m_screen_obj = NULL,
        .m_setting_data = {.output_voltage_mv = 100.0, .frequency = 100, .fan_mode = "100%%", .str_best_diff = "0M", .hashrate = 100.0, 
            .pool_url = "", .pool_port = 100,  .total_shares = 0, .efficency = 100, .str_ip = "0.0.0.0"
        }, 
        .m_image_element = {.image_dsc = &settingsscreen2, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE},
        .m_ui_element_array = {
            {.name="output_voltage_mv", .text="100.0", .unit_text="mV", .x_pos=63, .y_pos=-35, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="frequency", .text="100", .unit_text="Mhz", .x_pos=63, .y_pos=-15, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="fan_mode", .text="100%", .unit_text="", .x_pos=63, .y_pos=5, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="str_best_diff", .text="50M", .unit_text="", .x_pos=65, .y_pos=25, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x0, 0x0, 0x0)},
            {.name="hashrate", .text="500,0", .unit_text="", .x_pos=-210, .y_pos=61, .text_font=&lv_font_montserrat_28,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x0, 0x0, 0x0)},

            {.name="str_ip", .text="192.168.1.200", .unit_text="", .x_pos=-16, .y_pos=-76, .text_font=&lv_font_montserrat_10,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x0, 0x0, 0x0)},
            {.name="pool_url", .text="", .unit_text="", .x_pos=195, .y_pos=6, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="pool_port", .text="100", .unit_text="", .x_pos=230, .y_pos=23, .text_font=&lv_font_montserrat_12,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="shares", .text="100", .unit_text="", .x_pos=190, .y_pos=62, .text_font=&lv_font_montserrat_12,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)},
            {.name="efficency", .text="0/0", .unit_text="", .x_pos=-22, .y_pos=62, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0x00, 0x00, 0x00)}
        }
    },
    .m_doge_screen = {
        .m_screen_obj = NULL,
        .m_doge_screen_data = { 
            .hashrate = 100, .temperature = 0, .ltc_price = "$--", .block_num = 0
        }, 
        .m_image_element = {.image_dsc = &btcscreen, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE},
        .m_ui_element_array = {
            {.name="hashrate", .text="100.0", .unit_text="", .x_pos=-25, .y_pos=-52, .text_font=&lv_font_montserrat_20,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0x0, 0x0, 0x0)},
            {.name="temperature", .text="0", .unit_text="", .x_pos=-35, .y_pos=-10, .text_font=&lv_font_montserrat_20,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0x0, 0x0, 0x0)},
             {.name="ltc_price", .text="$--", .unit_text="", .x_pos=182, .y_pos=55, .text_font=&lv_font_montserrat_28,.p_lv_obj = NULL, /*&ui_font_DigitalNumbers28*/
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0xDE, 0xDA, 0xDE)}
            #if APP_SUPPORT_FOUND_BLOCK
            ,
            {.name="block_num", .text="", .unit_text="", .x_pos=285, .y_pos=40, .text_font=&lv_font_montserrat_28,.p_lv_obj = NULL, /*&ui_font_DigitalNumbers28*/
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_LEFT_MID, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0x0, 0x0, 0x0)}
            #endif
        }
    },
    .m_bathwater_screen = {
        .m_screen_obj = NULL,
        .m_image_element = {.image_dsc = &bathwater, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE},
        .m_ui_element_array = {
            /* The jar occupies x 8..105 of the 320 px panel, so the text sits
             * in what is left: 196 px wide, centred on the right-hand half.
             * A fixed width rather than LV_SIZE_CONTENT, because that is what
             * makes the label wrap -- the facts are not all one line long. */
            {.name="fact", .text="", .unit_text="", .x_pos=52, .y_pos=0, .text_font=&lv_font_montserrat_20,.p_lv_obj = NULL,
            .width=196, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0xff, 0xff, 0xff)},
        }
    },
    .m_global_screen = {
        .m_screen_obj = NULL,
        .m_global_stats_data = {
            .global_diff = "", .global_hashrate = "", .ltc_blockheight = "",
            .halving_progress = "", .halving_blocks = "", .price = ""
        },
        .m_image_element = {.image_dsc = &netstats, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE},
        .m_ui_element_array = {
            /* price, large, in the open band under the BITCOIN/USD label */
            {.name="price", .text="$--", .unit_text="", .x_pos=0, .y_pos=-24, .text_font=&lv_font_montserrat_28,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0xff, 0xb8, 0x1c)},
            {.name="ltc_blockheight", .text="0", .unit_text="", .x_pos=-61, .y_pos=38, .text_font=&lv_font_montserrat_20,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0xff, 0xff, 0xff)},
            {.name="global_hashrate", .text="0", .unit_text=" EH/s", .x_pos=85, .y_pos=38, .text_font=&lv_font_montserrat_20,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0xff, 0xff, 0xff)}
        }
    },
    .m_splash_screen = {
        .m_screen_obj = NULL,
        .m_splash_data = {.note = "connecting"},
        .m_image_element = {.image_dsc = &splashscreen2, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE},
        .m_ui_element_array = {
            {.name="note", .text="connecting", .unit_text="", .x_pos=-40, .y_pos=27, .text_font=&lv_font_montserrat_12,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_RIGHT_MID, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0x0, 0x0, 0x0)},
            {.name="wifi_ssid", .text="GOKJ", .unit_text="", .x_pos=95, .y_pos=55, .text_font=&lv_font_montserrat_12,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0x0, 0x0, 0x0)}
        }
    },
    .m_log_screen = {
        .m_screen_obj = NULL,
        .m_image_element = {.image_dsc = NULL, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE},
        .m_ui_element_array = {
            {.name="log", .text="log", .unit_text="", .x_pos=0, .y_pos=0, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_OUT_LEFT_TOP, .text_align=LV_TEXT_ALIGN_LEFT, .text_color=LV_COLOR_MAKE(0xff, 0xff, 0xff)}
        }
    },
    .m_clock_screen = {
        .m_screen_obj = NULL,
        .m_clock_data = {.clock_time = "--:--", .clock_date = "", .hashrate = "0", .pool_split = ""},
        .m_image_element = {.image_dsc = &clockscreen, .align = LV_ALIGN_CENTER, .add_flag = LV_OBJ_FLAG_ADV_HITTEST, .clear_flag = LV_OBJ_FLAG_SCROLLABLE},
        .m_ui_element_array = {
            {.name="clock_time", .text="--:--", .unit_text="", .x_pos=0, .y_pos=-18, .text_font=&lv_font_montserrat_28,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0xff, 0xff, 0xff)},
            {.name="clock_date", .text="", .unit_text="", .x_pos=0, .y_pos=18, .text_font=&lv_font_montserrat_16,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0x96, 0xaa, 0xc8)},
            /* the miner's own rate, small, bottom right */
            {.name="hashrate", .text="0", .unit_text=" TH/s", .x_pos=88, .y_pos=61, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_RIGHT, .text_color=LV_COLOR_MAKE(0xff, 0xb8, 0x1c)},
            /* dual-pool split, under the date; blank when dual mining is off so
             * the page looks the same as before on a single-pool miner */
            {.name="pool_split", .text="", .unit_text="", .x_pos=0, .y_pos=37, .text_font=&lv_font_montserrat_14,.p_lv_obj = NULL,
            .width=LV_SIZE_CONTENT, .height=LV_SIZE_CONTENT, .align=LV_ALIGN_CENTER, .text_align=LV_TEXT_ALIGN_CENTER, .text_color=LV_COLOR_MAKE(0x96, 0xaa, 0xc8)}
        }
    }
};

void setup_text_style() {
    // 初始化样式
    lv_style_init(&style);   
    // 设置颜色（使用常量表达式）
    lv_style_set_text_color(&style, lv_color_make(0xDE, 0xDA, 0xDE));
    // 设置文本对齐方式
    lv_style_set_text_align(&style, LV_TEXT_ALIGN_LEFT);
    // 设置字体（运行时操作）
    lv_style_set_text_font(&style, &lv_font_montserrat_14);
}

lv_obj_t *create_image_obj(lv_obj_t *screen, ImageElement *image_element)
{
    lv_obj_t *img_obj = lv_img_create(screen);
    if(img_obj == NULL)
    {
        ESP_LOGW(TAG, "create_image_obj fail.");
        return NULL;
    }

    lv_img_set_src(img_obj, image_element->image_dsc);
    lv_obj_set_width(img_obj, image_element->image_dsc->header.w);
    lv_obj_set_height(img_obj, image_element->image_dsc->header.h);
    lv_obj_set_align(img_obj, image_element->align);
    lv_obj_add_flag(img_obj, image_element->add_flag);
    lv_obj_clear_flag(img_obj, image_element->clear_flag);
    image_element->p_image_obj = img_obj;

    ESP_LOGD(TAG, "create_image_obj done.");

    return img_obj;
}

void create_ui_elements(lv_obj_t *screen, UiElement *elements, size_t sizeofelements)
{
    if(NULL == elements){
        ESP_LOGW(TAG, "@0x%p error uielemnts.", screen);
        return;
    }

    for(size_t i = 0; i < sizeofelements; i++)
    {
        lv_obj_t *ui_obj = lv_label_create(screen);
        elements[i].screen_index = i + 1;
        /*lv_obj_set_name(ui_obj, elements[i].name);*/
        lv_obj_set_width(ui_obj, elements[i].width);
        lv_obj_set_height(ui_obj, elements[i].height);
        lv_obj_set_x(ui_obj, elements[i].x_pos);
        lv_obj_set_y(ui_obj, elements[i].y_pos);
        lv_obj_set_align(ui_obj, elements[i].align);

        lv_obj_set_style_text_font(ui_obj, elements[i].text_font, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(ui_obj, elements[i].text_align, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ui_obj, elements[i].text_color, LV_PART_MAIN | LV_STATE_DEFAULT);

        elements[i].p_lv_obj = ui_obj;
    }
}

void refresh_ui_elements(lv_obj_t *screen, UiElement *elements, size_t sizeofelements)
{
    if(NULL == elements){
        ESP_LOGW(TAG, "@0x%p error uielemnts.", screen);
        return;
    }
    
    for(size_t i = 0; i < sizeofelements; i++){
        lv_label_set_text(elements[i].p_lv_obj, elements[i].text);
    }
}

void refresh_one_elemet(lv_obj_t *screen, UiElement *elements, size_t sizeofelements, size_t element_index, ImageElement *image_element)
{
    if(NULL == elements){
        ESP_LOGW(TAG, "@0x%p error uielemnts.", screen);
        return;
    }

    size_t index = element_index % (sizeofelements+1);
    if(index < sizeofelements)
        lv_label_set_text(elements[index].p_lv_obj, elements[index].text);
    /*update the screen image.*/
    else if(index == sizeofelements){
        //lv_obj_mark_layout_as_dirty(image_element->p_image_obj);
        lv_obj_invalidate(screen);
        lv_refr_now(NULL);
#ifdef LVGL_PORTING_DEBUG        
        printf("reload the image.\n");
#endif
    }
}

void init_all_screen(lv_disp_t *disp)
{
    setup_text_style();

    /*create the msg queue.*/
    screen_msg_queue = xQueueCreate(10, sizeof(screen_msg));
}

bool format_seconds_to_string(uint32_t total_seconds, char* buffer, size_t buffer_size, bool show_seconds) {
    if (buffer == NULL || buffer_size < 12) {
        ESP_LOGE(TAG, "Invalid buffer or buffer too small");
        return false;
    }
    
    unsigned int  days = total_seconds / 86400;
    unsigned int  hours = (total_seconds % 86400) / 3600;
    unsigned int  minutes = (total_seconds % 3600) / 60;
    unsigned int  seconds = total_seconds % 60;
    
    int chars_written;
    
    if (days > 0) {
        if (show_seconds) {
            chars_written = snprintf(buffer, buffer_size, "%ud %uh %um %us", 
                                   days, hours, minutes, seconds);
        } else {
            chars_written = snprintf(buffer, buffer_size, "%ud %uh %um", 
                                   days, hours, minutes);
        }
    } else {
        if (show_seconds) {
            chars_written = snprintf(buffer, buffer_size, "%uh %um %us", 
                                   hours, minutes, seconds);
        } else {
            chars_written = snprintf(buffer, buffer_size, "%uh %um", 
                                   hours, minutes);
        }
    } 
    
    return (chars_written > 0 && (size_t)chars_written < buffer_size);
}

void refresh_mining_text()
{
    mining_screen *cur_mining_screen = &(volc_s3_display.m_mining_screen);
    
    snprintf(cur_mining_screen->m_ui_element_array[0].text, 20, "%.2f%s",
        cur_mining_screen->m_mining_screen_data.input_voltage_v, 
        cur_mining_screen->m_ui_element_array[0].unit_text);
    snprintf(cur_mining_screen->m_ui_element_array[1].text, 20, "%.0f%s",
        cur_mining_screen->m_mining_screen_data.output_voltage_mv*1000, 
        cur_mining_screen->m_ui_element_array[1].unit_text);
    snprintf(cur_mining_screen->m_ui_element_array[2].text, 20, "%.0f%s",
        cur_mining_screen->m_mining_screen_data.output_current_ma, 
        cur_mining_screen->m_ui_element_array[2].unit_text);
    snprintf(cur_mining_screen->m_ui_element_array[3].text, 20, "%.1f%s",
        cur_mining_screen->m_mining_screen_data.output_power_w, 
        cur_mining_screen->m_ui_element_array[3].unit_text);
    snprintf(cur_mining_screen->m_ui_element_array[4].text, 20, "%.2f%s",
        cur_mining_screen->m_mining_screen_data.efficiency_w_per_m, 
        cur_mining_screen->m_ui_element_array[4].unit_text);

    snprintf(cur_mining_screen->m_ui_element_array[5].text, 20, "%d%s",
        cur_mining_screen->m_mining_screen_data.temperature, 
        cur_mining_screen->m_ui_element_array[5].unit_text);
    format_seconds_to_string((uint32_t)cur_mining_screen->m_mining_screen_data.uptime_seconds, 
        cur_mining_screen->m_ui_element_array[6].text, 30, true);

    strncpy(cur_mining_screen->m_ui_element_array[7].text, 
        cur_mining_screen->m_mining_screen_data.str_ip, 20);
    strncpy(cur_mining_screen->m_ui_element_array[8].text, 
        cur_mining_screen->m_mining_screen_data.str_best_diff, 20);
    snprintf(cur_mining_screen->m_ui_element_array[9].text, 20, "%.0f%s", 
        (float)(cur_mining_screen->m_mining_screen_data.hashrate), 
        cur_mining_screen->m_ui_element_array[9].unit_text);

    snprintf(cur_mining_screen->m_ui_element_array[10].text, 20, "%u%s", 
        cur_mining_screen->m_mining_screen_data.rpm, 
        cur_mining_screen->m_ui_element_array[10].unit_text);
    strncpy(cur_mining_screen->m_ui_element_array[11].text, 
        cur_mining_screen->m_mining_screen_data.str_asic_model, 20);
    strncpy(cur_mining_screen->m_ui_element_array[12].text, 
        cur_mining_screen->m_mining_screen_data.model_str, 10);
    refresh_ip_from_system();
}

void refresh_mining_screen(bool b_load_screen)
{
    mining_screen *cur_mining_screen = &(volc_s3_display.m_mining_screen);
    #if defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    static uint32_t counter = 0;
    #endif

    if(NULL == cur_mining_screen)
        return;

    if(NULL == cur_mining_screen->m_screen_obj){
        /*create the screen.*/
    #if 0    
        //cur_mining_screen->m_screen_obj = lv_display_get_screen_active(disp);
    #else
        cur_mining_screen->m_screen_obj = lv_obj_create(NULL);
    #endif
        //lv_obj_set_name(cur_mining_screen->m_screen_obj, "mining_screen");        
        /*apply the the style.*/
        lv_obj_add_style(cur_mining_screen->m_screen_obj, &style, LV_PART_MAIN|LV_STATE_DEFAULT);
        /*set the image.*/
        create_image_obj(cur_mining_screen->m_screen_obj, &(cur_mining_screen->m_image_element));
        create_ui_elements(
            cur_mining_screen->m_screen_obj, cur_mining_screen->m_ui_element_array, 
            sizeof(cur_mining_screen->m_ui_element_array)/sizeof(cur_mining_screen->m_ui_element_array[0])
        );
        refresh_ui_elements(cur_mining_screen->m_screen_obj, cur_mining_screen->m_ui_element_array,
            sizeof(cur_mining_screen->m_ui_element_array)/sizeof(cur_mining_screen->m_ui_element_array[0])
        );

        /*set the image wifi.*/
        // Create a container for the overlay
        lv_obj_t *wifi_container = lv_obj_create(cur_mining_screen->m_screen_obj);
        lv_obj_set_size(wifi_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT); // Size will fit image
        //lv_obj_align(wifi_container, LV_ALIGN_CENTER, 0, 0); // Center the overlay on the screen
        lv_obj_set_x(wifi_container, 284);
        lv_obj_set_y(wifi_container, -9); 

        // Disable scrollbars for the container
        lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_SCROLLABLE);

        // Optional: make background transparent or keep style minimal
        lv_obj_set_style_bg_opa(wifi_container, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(wifi_container, 0, LV_PART_MAIN);

        // Create an image inside the container
        lv_obj_t *img = lv_img_create(wifi_container);
        lv_img_set_src(img, &wifi4);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        cur_mining_screen->m_image_element_wifi.p_image_obj = img;
    }
    refresh_mining_text();

    const lv_image_dsc_t *image_dsc;
    NetWorkInfo netWork_Info = network_get_info();
    if(netWork_Info.eth_get_ip)
    {
        image_dsc = &eth_2;
    }
    else
    {
        get_wifi_current_rssi(&(cur_mining_screen->m_mining_screen_data.rssi));
        if(cur_mining_screen->m_mining_screen_data.rssi >= -50)
        {
            image_dsc = &wifi4;
        }
        else if(cur_mining_screen->m_mining_screen_data.rssi >= -60)
        {
            image_dsc = &wifi3;
        }
        else if(cur_mining_screen->m_mining_screen_data.rssi >= -70)
        {
            image_dsc = &wifi2;
        }
        else
        {
            image_dsc = &wifi1;
        }
    }
    lv_img_set_src(cur_mining_screen->m_image_element_wifi.p_image_obj,image_dsc);

#if defined(REFRESH_UI_ELEMENTS_TOGOTHRE)    
    refresh_ui_elements(cur_mining_screen->m_screen_obj, cur_mining_screen->m_ui_element_array,
        sizeof(cur_mining_screen->m_ui_element_array)/sizeof(cur_mining_screen->m_ui_element_array[0])
    );
#elif defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    refresh_one_elemet(cur_mining_screen->m_screen_obj, cur_mining_screen->m_ui_element_array,
        sizeof(cur_mining_screen->m_ui_element_array)/sizeof(cur_mining_screen->m_ui_element_array[0]), 
        counter++, &(cur_mining_screen->m_image_element));
#endif

#ifdef LVGL_PORTING_DEBUG    
    printf("refresh_mining_screen\n");
#endif

    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN)    
        lv_screen_load(cur_mining_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
        cur_mining_screen->m_screen_obj,
        LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
    );
#endif
    }
}

void refresh_portal_text()
{
    portal_screen *cur_portal_screen = &(volc_s3_display.m_portal_screen);
    
    strncpy(cur_portal_screen->m_ui_element_array[0].text, cur_portal_screen->m_portal_screen_data.ssid, 20);
}

void refresh_portal_screen(bool b_load_screen)
{
    portal_screen *cur_portal_screen = &(volc_s3_display.m_portal_screen);

    if(NULL == cur_portal_screen)
        return;
    
    if(NULL == cur_portal_screen->m_screen_obj){
        /*create the screen.*/
#if 0       
        cur_portal_screen->m_screen_obj = lv_display_get_screen_active(disp);
#else
        cur_portal_screen->m_screen_obj = lv_obj_create(NULL);
#endif
        //lv_obj_set_name(cur_portal_screen->m_screen_obj, "mining_screen"); 
        /*apply the the style.*/
        lv_obj_add_style(cur_portal_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        /*set the image*/
        create_image_obj(cur_portal_screen->m_screen_obj, &(cur_portal_screen->m_image_element));
        create_ui_elements(
            cur_portal_screen->m_screen_obj, cur_portal_screen->m_ui_element_array, 
            sizeof(cur_portal_screen->m_ui_element_array)/sizeof(cur_portal_screen->m_ui_element_array[0])
        );

        if(NULL == cur_portal_screen->m_qr_wifi_obj)
        {
            cur_portal_screen->m_qr_wifi_obj = lv_obj_create(cur_portal_screen->m_screen_obj);
            lv_obj_set_size(cur_portal_screen->m_qr_wifi_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT); // Size will fit image
            lv_obj_align(cur_portal_screen->m_qr_wifi_obj, LV_ALIGN_DEFAULT, 25, 39); // Center the overlay on the screen

            // Disable scrollbars for the container
            lv_obj_clear_flag(cur_portal_screen->m_qr_wifi_obj, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_set_style_bg_color(cur_portal_screen->m_qr_wifi_obj, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
            // Optional: make background transparent or keep style minimal
            lv_obj_set_style_bg_opa(cur_portal_screen->m_qr_wifi_obj, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(cur_portal_screen->m_qr_wifi_obj, 0, LV_PART_MAIN);

            cur_portal_screen->m_qr_wifi = lv_qrcode_create(cur_portal_screen->m_qr_wifi_obj);

            lv_qrcode_set_size(cur_portal_screen->m_qr_wifi,80);
            //lv_qrcode_update(cur_portal_screen->m_qr_wifi, cur_portal_screen->m_portal_screen_data.ssid, strlen(cur_portal_screen->m_portal_screen_data.ssid));
        }
    }
    
    refresh_portal_text();
    refresh_ui_elements(cur_portal_screen->m_screen_obj, cur_portal_screen->m_ui_element_array,
        sizeof(cur_portal_screen->m_ui_element_array)/sizeof(cur_portal_screen->m_ui_element_array[0])
    );
    char wifi_str[38];
    sprintf(wifi_str,"WIFI:T:nopass;S:%s;",cur_portal_screen->m_portal_screen_data.ssid);
    lv_qrcode_update(cur_portal_screen->m_qr_wifi, wifi_str, strlen(wifi_str));
    
    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN)
        lv_screen_load(cur_portal_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
        cur_portal_screen->m_screen_obj,
        LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
    );
#endif
    }
}

void refresh_init_screen(bool b_load_screen)
{
    init2_screen *cur_init_screen = &(volc_s3_display.m_init2_screen);

    if(NULL == cur_init_screen)
        return;
    
    if(NULL == cur_init_screen->m_screen_obj){
        cur_init_screen->m_screen_obj = lv_obj_create(NULL);
        lv_obj_add_style(cur_init_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        create_image_obj(cur_init_screen->m_screen_obj, &(cur_init_screen->m_image_element));
    }

    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN)    
        lv_screen_load(cur_init_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
            cur_init_screen->m_screen_obj,
            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
        );
#endif    
    }
}

void refresh_splash_text()
{
    splash_screen *cur_splash_screen = &(volc_s3_display.m_splash_screen);
    char * wifi_ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, CONFIG_ESP_WIFI_SSID);
    if(wifi_ssid)
    {
        strncpy(cur_splash_screen->m_ui_element_array[1].text, wifi_ssid, 20);
        free(wifi_ssid);
    }

    strncpy(cur_splash_screen->m_ui_element_array[0].text, cur_splash_screen->m_splash_data.note, 20);
}

void refresh_splash_screen(bool b_load_screen)
{
    splash_screen *cur_splash_screen = &(volc_s3_display.m_splash_screen);

    if(NULL == cur_splash_screen)
        return;

    if(NULL == cur_splash_screen->m_screen_obj){
        cur_splash_screen->m_screen_obj = lv_obj_create(NULL);
        lv_obj_add_style(cur_splash_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        create_image_obj(cur_splash_screen->m_screen_obj, &(cur_splash_screen->m_image_element));

        create_ui_elements(
            cur_splash_screen->m_screen_obj, cur_splash_screen->m_ui_element_array,
            sizeof(cur_splash_screen->m_ui_element_array)/sizeof(cur_splash_screen->m_ui_element_array[0])
        );       
    }

    refresh_splash_text();
    refresh_ui_elements(cur_splash_screen->m_screen_obj, cur_splash_screen->m_ui_element_array,
        sizeof(cur_splash_screen->m_ui_element_array)/sizeof(cur_splash_screen->m_ui_element_array[0])
    );

    /*no text on splash screen, do nothing.*/
    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN)        
        lv_screen_load(cur_splash_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
            cur_splash_screen->m_screen_obj,
            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
        );
#endif
    }
}

void refresh_log_screen(bool b_load_screen)
{
    log_screen *cur_log_screen = &(volc_s3_display.m_log_screen);

    if(NULL == cur_log_screen)
        return;

    if(NULL == cur_log_screen->m_screen_obj){
        cur_log_screen->m_screen_obj = lv_obj_create(NULL);
        lv_obj_add_style(cur_log_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        // Create a black background
        lv_obj_set_style_bg_color(cur_log_screen->m_screen_obj, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(cur_log_screen->m_screen_obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

        create_ui_elements(
            cur_log_screen->m_screen_obj, cur_log_screen->m_ui_element_array,
            sizeof(cur_log_screen->m_ui_element_array)/sizeof(cur_log_screen->m_ui_element_array[0])
        );       
    }

    /*no text on splash screen, do nothing.*/
    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN)        
        lv_screen_load(cur_log_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
            cur_log_screen->m_screen_obj,
            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
        );
#endif
    }
}

void logMessage(const char *message)
{
    log_screen *cur_log_screen = &(volc_s3_display.m_log_screen);

    if(NULL == cur_log_screen)
        return;

    if(NULL == cur_log_screen->m_screen_obj){
        cur_log_screen->m_screen_obj = lv_obj_create(NULL);
        lv_obj_add_style(cur_log_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        // Create a black background
        lv_obj_set_style_bg_color(cur_log_screen->m_screen_obj, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(cur_log_screen->m_screen_obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

        create_ui_elements(
            cur_log_screen->m_screen_obj, cur_log_screen->m_ui_element_array,
            sizeof(cur_log_screen->m_ui_element_array)/sizeof(cur_log_screen->m_ui_element_array[0])
        );       
    }

    if(message == NULL)
        return;

    lv_label_set_text(cur_log_screen->m_ui_element_array[0].p_lv_obj, message);

    cur_screen_index = SCREEN_LOG_SCREEN;
    b_screen_changed = true;
}

void refresh_global_text()
{
    UiElement *p_uielement_array = volc_s3_display.m_global_screen.m_ui_element_array;
    global_stats_data *cur_global_data = &(volc_s3_display.m_global_screen.m_global_stats_data);

    /* price large, then block height and network hashrate beneath it.
     * unit_text is only ever shown where a refresh function appends it, so
     * the suffix has to be joined on here or it never reaches the screen. */
    strncpy(p_uielement_array[0].text, cur_global_data->price, 20);
    strncpy(p_uielement_array[1].text, cur_global_data->ltc_blockheight, 20);
    snprintf(p_uielement_array[2].text, sizeof(p_uielement_array[2].text), "%s%s",
             cur_global_data->global_hashrate, p_uielement_array[2].unit_text);
}

void refresh_global_screen(bool b_load_screen)
{
    global_stats_screen *cur_global_screen = &(volc_s3_display.m_global_screen);
    #if defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    static uint32_t counter = 0;
    #endif

    if(NULL == cur_global_screen)
        return;
    
    if(NULL == cur_global_screen->m_screen_obj){
        cur_global_screen->m_screen_obj = lv_obj_create(NULL);
        /*apply the the style.*/
        lv_obj_add_style(cur_global_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        /*set the image*/
        create_image_obj(cur_global_screen->m_screen_obj, &(cur_global_screen->m_image_element));
        create_ui_elements(
            cur_global_screen->m_screen_obj, cur_global_screen->m_ui_element_array,
            sizeof(cur_global_screen->m_ui_element_array)/sizeof(cur_global_screen->m_ui_element_array[0])
        );
    }

    refresh_global_text();

#if defined(REFRESH_UI_ELEMENTS_TOGOTHRE)
    refresh_ui_elements(cur_global_screen->m_screen_obj, cur_global_screen->m_ui_element_array,
        sizeof(cur_global_screen->m_ui_element_array)/sizeof(cur_global_screen->m_ui_element_array[0])
    );
#elif defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    refresh_one_elemet(cur_global_screen->m_screen_obj, cur_global_screen->m_ui_element_array,
        sizeof(cur_global_screen->m_ui_element_array)/sizeof(cur_global_screen->m_ui_element_array[0]), counter++, &(cur_global_screen->m_image_element)
    );
#endif

    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN)        
        lv_screen_load(cur_global_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
            cur_global_screen->m_screen_obj,
            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
        );
#endif
    }
}

/*
 * Local time, with the miner's own rate small in the corner.
 *
 * The zone comes from NVS_CONFIG_TIME_ZONE, applied by rtc_sync() at start-up,
 * so this only has to read the clock -- but a device whose zone was never set
 * shows UTC, which looks like a bug rather than a setting.
 */
void refresh_clock_text()
{
    UiElement *p_uielement_array = volc_s3_display.m_clock_screen.m_ui_element_array;
    clock_screen_data *cur_clock_data = &(volc_s3_display.m_clock_screen.m_clock_data);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year > (2020 - 1900)) {
        strftime(cur_clock_data->clock_time, sizeof(cur_clock_data->clock_time), "%H:%M", &timeinfo);
        strftime(cur_clock_data->clock_date, sizeof(cur_clock_data->clock_date), "%a %d %b", &timeinfo);
    } else {
        /* NTP has not landed yet; say so rather than showing 1970 */
        strncpy(cur_clock_data->clock_time, "--:--", sizeof(cur_clock_data->clock_time));
        strncpy(cur_clock_data->clock_date, "no time sync", sizeof(cur_clock_data->clock_date));
    }

    snprintf(cur_clock_data->hashrate, sizeof(cur_clock_data->hashrate), "%.2f",
             volc_s3_display.m_mining_screen.m_mining_screen_data.hashrate / 1000.0);

    strncpy(p_uielement_array[0].text, cur_clock_data->clock_time, 20);
    strncpy(p_uielement_array[1].text, cur_clock_data->clock_date, 20);
    snprintf(p_uielement_array[2].text, sizeof(p_uielement_array[2].text), "%s%s",
             cur_clock_data->hashrate, p_uielement_array[2].unit_text);

    /* Where the hashrate is going, when it is going to two places. */
    if (volc_s3_display.m_dual_enable) {
        snprintf(cur_clock_data->pool_split, sizeof(cur_clock_data->pool_split),
                 "A %u%%  /  B %u%%", volc_s3_display.m_dual_ratio_a,
                 (unsigned)(100 - volc_s3_display.m_dual_ratio_a));
    } else {
        cur_clock_data->pool_split[0] = ' ';
    }
    strncpy(p_uielement_array[3].text, cur_clock_data->pool_split, 20);
}

void refresh_clock_screen(bool b_load_screen)
{
    clock_screen *cur_clock_screen = &(volc_s3_display.m_clock_screen);
#if defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    static uint32_t counter = 0;
#endif

    if(NULL == cur_clock_screen)
        return;

    if(NULL == cur_clock_screen->m_screen_obj){
        cur_clock_screen->m_screen_obj = lv_obj_create(NULL);
        lv_obj_add_style(cur_clock_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        create_image_obj(cur_clock_screen->m_screen_obj, &(cur_clock_screen->m_image_element));
        create_ui_elements(
            cur_clock_screen->m_screen_obj, cur_clock_screen->m_ui_element_array,
            sizeof(cur_clock_screen->m_ui_element_array)/sizeof(cur_clock_screen->m_ui_element_array[0])
        );
    }

    refresh_clock_text();

#if defined(REFRESH_UI_ELEMENTS_TOGOTHRE)
    refresh_ui_elements(cur_clock_screen->m_screen_obj, cur_clock_screen->m_ui_element_array,
        sizeof(cur_clock_screen->m_ui_element_array)/sizeof(cur_clock_screen->m_ui_element_array[0])
    );
#elif defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    refresh_one_elemet(cur_clock_screen->m_screen_obj, cur_clock_screen->m_ui_element_array,
        sizeof(cur_clock_screen->m_ui_element_array)/sizeof(cur_clock_screen->m_ui_element_array[0]), counter++, &(cur_clock_screen->m_image_element)
    );
#endif

    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN)
        lv_screen_load(cur_clock_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
            cur_clock_screen->m_screen_obj,
            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
        );
#endif
    }
}

void refresh_doge_text()
{
    UiElement *p_uielement_array = volc_s3_display.m_doge_screen.m_ui_element_array;
    doge_screen_data *cur_doge_data = &(volc_s3_display.m_doge_screen.m_doge_screen_data);

    snprintf(p_uielement_array[0].text, 20, "%.0f", cur_doge_data->hashrate);   
    snprintf(p_uielement_array[1].text, 20, "%d", cur_doge_data->temperature);
    strncpy(p_uielement_array[2].text, cur_doge_data->ltc_price, 20); 
    snprintf(p_uielement_array[3].text, 20, "%d", cur_doge_data->block_num);
}

/*
 * The jar screen. No data arrives for this one -- it exists to be looked at --
 * so the facts are held here and advanced by the screen's own refresh, which
 * runs about every EXAMPLE_LVGL_REFRESH_SCREEN_INTERVAL while it is on top.
 * Three refreshes per fact puts a change at roughly six seconds, which is long
 * enough to read a short line and short enough not to look stuck.
 */
static const char *k_bathwater_facts[] = {
    "Only 21 Million",
    "Decentralized",
    "Proof of Work",
    "Halving every\n210,000 blocks",
    "A block every\n10 minutes",
    "Genesis block\n3 Jan 2009",
    "190 Proof Whiskey\nRPM Bath Water",
};

void refresh_bathwater_screen(bool b_load_screen)
{
    bathwater_screen *cur = &(volc_s3_display.m_bathwater_screen);
    static uint32_t tick = 0;
    static size_t shown = (size_t)-1;
    const size_t count = sizeof(k_bathwater_facts)/sizeof(k_bathwater_facts[0]);

    if(NULL == cur)
        return;

    if(NULL == cur->m_screen_obj){
        cur->m_screen_obj = lv_obj_create(NULL);
        lv_obj_add_style(cur->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        create_image_obj(cur->m_screen_obj, &(cur->m_image_element));
        create_ui_elements(
            cur->m_screen_obj, cur->m_ui_element_array,
            sizeof(cur->m_ui_element_array)/sizeof(cur->m_ui_element_array[0])
        );
        tick = 0;
        shown = (size_t)-1;
    }

    /* Start on the first fact whenever the screen is entered, so it does not
     * resume mid-list showing whatever happened to be up last time. */
    if(b_load_screen){
        tick = 0;
    }

    size_t want = (tick / 3) % count;
    if(want != shown){
        lv_label_set_text(cur->m_ui_element_array[0].p_lv_obj, k_bathwater_facts[want]);
        shown = want;
    }
    tick++;

    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN)
        lv_screen_load(cur->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
            cur->m_screen_obj,
            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
        );
#endif
    }
}

void refresh_doge_screen(bool b_load_screen)
{
    doge_screen *cur_doge_screen = &(volc_s3_display.m_doge_screen);
    #if defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    static uint32_t counter = 0;
    #endif

    if(NULL == cur_doge_screen)
        return;

    if(NULL == cur_doge_screen->m_screen_obj){
        cur_doge_screen->m_screen_obj = lv_obj_create(NULL);
        /*apply the the style.*/
        lv_obj_add_style(cur_doge_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        /*set the image*/
        create_image_obj(cur_doge_screen->m_screen_obj, &(cur_doge_screen->m_image_element));
        create_ui_elements(
            cur_doge_screen->m_screen_obj, cur_doge_screen->m_ui_element_array,
            sizeof(cur_doge_screen->m_ui_element_array)/sizeof(cur_doge_screen->m_ui_element_array[0])
        );
    }

    refresh_doge_text();
#if defined(REFRESH_UI_ELEMENTS_TOGOTHRE)
    refresh_ui_elements(cur_doge_screen->m_screen_obj, cur_doge_screen->m_ui_element_array, 
        sizeof(cur_doge_screen->m_ui_element_array)/sizeof(cur_doge_screen->m_ui_element_array[0])
    );
#elif defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    refresh_one_elemet(cur_doge_screen->m_screen_obj, cur_doge_screen->m_ui_element_array, 
        sizeof(cur_doge_screen->m_ui_element_array)/sizeof(cur_doge_screen->m_ui_element_array[0]), 
        counter++, &(cur_doge_screen->m_image_element)
    );
#endif

    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN) 
        lv_screen_load(cur_doge_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
            cur_doge_screen->m_screen_obj,
            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
        );
#endif
    }
}

void refresh_setting_text()
{
    UiElement *p_uielement_array = volc_s3_display.m_setting_screen.m_ui_element_array;
    setting_data *cur_setting_data = &(volc_s3_display.m_setting_screen.m_setting_data);

    snprintf(p_uielement_array[0].text, 20, "%.0f%s", cur_setting_data->output_voltage_mv*1000, p_uielement_array[0].unit_text);
    snprintf(p_uielement_array[1].text, 20, "%u%s", cur_setting_data->frequency, p_uielement_array[1].unit_text);
    snprintf(p_uielement_array[2].text, sizeof(p_uielement_array[2].text), "%s", cur_setting_data->fan_mode);
    snprintf(p_uielement_array[3].text, sizeof(p_uielement_array[3].text), "%s", cur_setting_data->str_best_diff);
    snprintf(p_uielement_array[4].text, 20, "%.0f", cur_setting_data->hashrate);
    snprintf(p_uielement_array[5].text, sizeof(p_uielement_array[5].text), "%s", cur_setting_data->str_ip);
    snprintf(p_uielement_array[6].text, sizeof(p_uielement_array[6].text), "%s", cur_setting_data->pool_url);
    snprintf(p_uielement_array[7].text, 20, "%u", cur_setting_data->pool_port);
    snprintf(p_uielement_array[8].text, 20, "%"PRIu32"", cur_setting_data->total_shares);
    snprintf(p_uielement_array[9].text, 20, "%.2f", cur_setting_data->efficency);
}

void refresh_setting_screen(bool b_load_screen)
{
    setting_screen *cur_setting_screen = &(volc_s3_display.m_setting_screen);
    #if defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    static uint32_t counter = 0;
    #endif

    if(NULL == cur_setting_screen)
        return;
    
    if(NULL == cur_setting_screen->m_screen_obj){
        cur_setting_screen->m_screen_obj = lv_obj_create(NULL);
        /*apply the style.*/
        lv_obj_add_style(cur_setting_screen->m_screen_obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT);
        /*set the image*/
        create_image_obj(cur_setting_screen->m_screen_obj, &(cur_setting_screen->m_image_element));
        create_ui_elements(
            cur_setting_screen->m_screen_obj, cur_setting_screen->m_ui_element_array,
            sizeof(cur_setting_screen->m_ui_element_array)/sizeof(cur_setting_screen->m_ui_element_array[0])
        );
        refresh_ui_elements(cur_setting_screen->m_screen_obj, cur_setting_screen->m_ui_element_array,
            sizeof(cur_setting_screen->m_ui_element_array)/sizeof(cur_setting_screen->m_ui_element_array[0])
        );

        /*set the image wifi.*/
        // Create a container for the overlay
        lv_obj_t *wifi_container = lv_obj_create(cur_setting_screen->m_screen_obj);
        lv_obj_set_size(wifi_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT); // Size will fit image
        //lv_obj_align(wifi_container, LV_ALIGN_CENTER, 0, 0); // Center the overlay on the screen
        lv_obj_set_x(wifi_container, 183);
        lv_obj_set_y(wifi_container, -9); 

        // Disable scrollbars for the container
        lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_SCROLLABLE);

        // Optional: make background transparent or keep style minimal
        lv_obj_set_style_bg_opa(wifi_container, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(wifi_container, 0, LV_PART_MAIN);

        // Create an image inside the container
        lv_obj_t *img = lv_img_create(wifi_container);
        lv_img_set_src(img, &wifi4);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        cur_setting_screen->m_image_element_wifi.p_image_obj = img;
    }

    refresh_setting_text();

    const lv_image_dsc_t *image_dsc;
    NetWorkInfo netWork_Info = network_get_info();
    if((netWork_Info.eth_get_ip && netWork_Info.wifi_get_ip) || netWork_Info.wifi_get_ip)
    {
        get_wifi_current_rssi(&(cur_setting_screen->m_setting_data.rssi));
        if(cur_setting_screen->m_setting_data.rssi >= -50)
        {
            image_dsc = &wifi4;
        }
        else if(cur_setting_screen->m_setting_data.rssi >= -60)
        {
            image_dsc = &wifi3;
        }
        else if(cur_setting_screen->m_setting_data.rssi >= -70)
        {
            image_dsc = &wifi2;
        }
        else
        {
            image_dsc = &wifi1;
        }
    }
    else if(netWork_Info.eth_get_ip)
    {
        image_dsc = &eth_2;
    }
    else
    {
        image_dsc = NULL;
    }
    lv_img_set_src(cur_setting_screen->m_image_element_wifi.p_image_obj,image_dsc);


#if defined(REFRESH_UI_ELEMENTS_TOGOTHRE)
    refresh_ui_elements(cur_setting_screen->m_screen_obj, cur_setting_screen->m_ui_element_array,
        sizeof(cur_setting_screen->m_ui_element_array)/sizeof(cur_setting_screen->m_ui_element_array[0]));
#elif defined(REFRESH_UI_ELEMENTS_ONE_BY_ONE)
    refresh_one_elemet(cur_setting_screen->m_screen_obj, cur_setting_screen->m_ui_element_array,
        sizeof(cur_setting_screen->m_ui_element_array)/sizeof(cur_setting_screen->m_ui_element_array[0]), 
        counter++, &(cur_setting_screen->m_image_element)
    );
#endif

    if(b_load_screen){
#if defined(LEGACY_LOAD_SCREEN) 
        lv_screen_load(cur_setting_screen->m_screen_obj);
#elif defined(ANIMATION_LOAD_SCREEN)
        lv_screen_load_anim(
            cur_setting_screen->m_screen_obj,
            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, LV_DEF_REFR_PERIOD*128/8, 0, false
        );
#endif
    }
}

/*
 * Button order: mining, then network stats, then the clock, then back.
 *
 * The vendor cycle ran global -> coin -> mining -> settings, which opened on a
 * stats page rather than on the thing the device is for. Settings is still
 * built and refreshed; it is simply not in the button rotation, and the same
 * information is on the web UI. Putting it back is one line here.
 */
void mining_next_screen()
{
    switch(cur_screen_index){
        case SCREEN_MINING_SCREEN:
            cur_screen_index = SCREEN_GLOBAL_SCREEN;
            break;
        case SCREEN_GLOBAL_SCREEN:
            cur_screen_index = SCREEN_CLOCK_SCREEN;
            break;
        case SCREEN_CLOCK_SCREEN:
            cur_screen_index = SCREEN_BATHWATER_SCREEN;
            break;
        default:
            cur_screen_index = SCREEN_MINING_SCREEN;
            break;
    }
    return;
}

void display_backlight_on()
{
    gpio_set_level(EXAMPLE_PIN_NUM_PWR, true);
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);
}

void display_backlight_off()
{
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL);
    gpio_set_level(EXAMPLE_PIN_NUM_PWR, false);
}

void display_backlight_invert()
{
    display_on_off = !display_on_off;
    
    if(!display_on_off)
        display_backlight_off();
    else
        display_backlight_on();
}

void inform_wifi_status(bool b_connected)
{
    screen_msg msg;

    msg.msg_type = WIFI_STATE_CONNECTED;
    if(b_connected){
        strncpy(msg.update_msg, "connected", 20);
    }else{
        strncpy(msg.update_msg, "fail", 20);
    }

    xQueueSend(screen_msg_queue, &msg, 0);
}

void inform_mining_status()
{
    screen_msg msg = {.msg_type = UPDATE_MINING_STATUS};
    xQueueSend(screen_msg_queue, &msg, 0);
}

bool parse_wifi_connected_msg(const char* update_msg)
{
    bool connected = false;

    if(0 == strncmp(update_msg, "connected", strlen("connected"))){
        connected = true;
    }

    return connected;
}

void show_first_screen()
{
    //refresh_init_screen(true);
    cur_screen_index = SCREEN_INIT_SCREEN;
    b_screen_changed = true;
    lv_tick_inc(EXAMPLE_LVGL_REFRESH_SCREEN_INTERVAL);
}

void refresh_current_screen()
{   
#ifdef LVGL_PORTING_DEBUG
    printf("refresh screen %d.\n", cur_screen_index);
#endif
    
    switch (cur_screen_index){
        case SCREEN_INIT_SCREEN:
            refresh_init_screen(b_screen_changed);
            break;
        case SCREEN_PORTAL_SCREEN:
            refresh_portal_screen(b_screen_changed);
            break;
        case SCREEN_SPLASH_SCREEN:
            refresh_splash_screen(b_screen_changed);
            break;
        case SCREEN_GLOBAL_SCREEN:
            refresh_global_screen(b_screen_changed);
            break;        
        case SCREEN_DOGE_SCREEN:
            refresh_doge_screen(b_screen_changed);
            break;
        case SCREEN_BATHWATER_SCREEN:
            refresh_bathwater_screen(b_screen_changed);
            break;
        case SCREEN_MINING_SCREEN:
            refresh_mining_screen(b_screen_changed);
            break;   
        case SCREEN_SETTING_SCREEN:
            refresh_setting_screen(b_screen_changed);
            break;
        case SCREEN_CLOCK_SCREEN:
            refresh_clock_screen(b_screen_changed);
            break;      
        case SCREEN_LOG_SCREEN:
            refresh_log_screen(b_screen_changed);
            break; 
        default:
            break;
    }

    if(b_screen_changed){
        b_screen_changed = false;
    }
}

void check_multi_press(void)
{
    static int press_count = 0;
    static uint32_t time_last = 0;

    uint32_t now = esp_timer_get_time()/1000;

    if((now - time_last) < 480)
    {
        press_count ++;
        if(press_count == 5)
        {
            showQrScreen();
        }
    }
    else
    {
        press_count = 0;
    }
    time_last = now;
    //ESP_LOGI(TAG, "Press %d",press_count);
}

void lv_update_screen_cb()
{
    screen_msg msg;
    /*wait the creation of screen_msg_queue.*/
    if(NULL == screen_msg_queue)
        return;        

    /*Get all the msg*/
    while(pdPASS == xQueueReceive(screen_msg_queue, &msg, 0)){
        switch (screen_status)
        {
            case STATS_BOOT_UP:
            case STATS_WIFI_CONNECTING:
                ESP_LOGD(TAG, "Boot or Wifi connecting: Get msg_type %d %s.", 
                    msg.msg_type, msg.update_msg);

                if(WIFI_STATE_CONNECTED == msg.msg_type){
                    if(true == parse_wifi_connected_msg(msg.update_msg)){
                        ESP_LOGI(TAG, "Change to splash screen");
                        screen_status = STATS_INIT;
                        cur_screen_index = SCREEN_SPLASH_SCREEN;
                        b_screen_changed = true;
                        refresh_ip_from_system();
                        lv_tick_inc(EXAMPLE_LVGL_REFRESH_SCREEN_INTERVAL);
                    }else{
                        ESP_LOGI(TAG, "Change to portal screen");
                        screen_status = STATS_WIFI_CONNECTING;
                        cur_screen_index = SCREEN_PORTAL_SCREEN;
                        b_screen_changed = true;
                        lv_tick_inc(EXAMPLE_LVGL_REFRESH_SCREEN_INTERVAL);
                    }
                }
                break;

            case STATS_INIT:
                ESP_LOGD(TAG, "Init: Get msg_type %d.", msg.msg_type);

                if(UPDATE_MINING_STATUS == msg.msg_type){
                    screen_status = STATS_MINING;
                    //cur_screen_index = SCREEN_MINING_SCREEN;
                    cur_screen_index = SCREEN_DOGE_SCREEN;
                    b_screen_changed = true;
                    lv_tick_inc(EXAMPLE_LVGL_REFRESH_SCREEN_INTERVAL);
                }
                break;

            case STATS_MINING:
                ESP_LOGD(TAG, "Mining: Get msg_type %d.", msg.msg_type);

                switch(msg.msg_type){
                    case NEXT_KEY_PREESSED:
                        hideErrorScreen();
                        hideFoundBlockScreen();
                        hideQrScreen();
                        mining_next_screen();
                        b_screen_changed = true;
                        lv_tick_inc(EXAMPLE_LVGL_REFRESH_SCREEN_INTERVAL);
                        break;            
                    case DISPLAY_KEY_PREESSED:
                        display_backlight_invert();
                        check_multi_press();
                        break;
                    case UPDATE_MINING_STATUS:
                        /*do nothing.*/
                        break;
                    case UPDATE_SETTINGS:
                        /*TODO: update the status.*/
                        break;
                    case BLOCK_FOUND:
                        /*TODO:*/
                        break;
                    case OVERHEATE_WARNING:
                        /*TODO*/
                        break;
                    case UPDATE_PRICE:
                        /*TODO*/
                        break;
                    case WIFI_STATE_CONNECTED:
                        if(false == parse_wifi_connected_msg(msg.update_msg))
                        {
                            ESP_LOGE(TAG, "Failed to parse WiFi connected message");
                            vTaskDelay(pdMS_TO_TICKS(500));
                            if(!network_is_connected())
                            restart_with_reason("Failed to parse WiFi connected message");
                        }
                        break;
                    default:
                        break;
                }
                break;

            default:
                break;
        }    
    }
}

extern _lock_t lvgl_api_lock;
void refresh_sensor_data(sensor_data new_sensor_data)
{
    /*refresh setting data, mining data and doge screen.*/
    _lock_acquire(&lvgl_api_lock);

    mining_screen_data *p_screen_data = &(volc_s3_display.m_mining_screen.m_mining_screen_data);
    p_screen_data->input_voltage_v = new_sensor_data.input_voltage_v;
    p_screen_data->output_voltage_mv = new_sensor_data.output_voltage_mv;
    p_screen_data->output_power_w = new_sensor_data.output_power_w;
    p_screen_data->output_current_ma = new_sensor_data.output_current_ma;
    p_screen_data->rpm = new_sensor_data.rpm;
    p_screen_data->temperature = new_sensor_data.temperature;
    p_screen_data->efficiency_w_per_m = new_sensor_data.efficency;

    setting_data *p_setting_data = &(volc_s3_display.m_setting_screen.m_setting_data);
    p_setting_data->output_voltage_mv = new_sensor_data.output_voltage_mv;
    strncpy(p_setting_data->fan_mode, new_sensor_data.fan_mode, 
        sizeof(p_setting_data->fan_mode)/sizeof(p_setting_data->fan_mode[0]));

    doge_screen_data *p_doge_data = &(volc_s3_display.m_doge_screen.m_doge_screen_data);
    p_doge_data->temperature = new_sensor_data.temperature;

    _lock_release(&lvgl_api_lock);
}

void refresh_sensor_data_from_system(GlobalState *GLOBAL_STATE)
{
    HealthMaintenceModule  *health_module = &(GLOBAL_STATE->HEALTH_MODULE);
    sensor_data new_sensor_data;

    new_sensor_data.input_voltage_v = health_module->input_voltage;
    new_sensor_data.output_current_ma = health_module->out_current > 0 ? health_module->out_current : 0;
    new_sensor_data.output_voltage_mv = health_module->out_voltage;
    new_sensor_data.output_power_w = health_module->power > 0 ? health_module->power : 0;
    new_sensor_data.temperature = health_module->board_temperature[0];
    new_sensor_data.rpm = health_module->fan_rpm[0];
    if(GLOBAL_STATE->SYSTEM_MODULE.current_hashrate>0.0001)
    new_sensor_data.efficency = health_module->power*1000/(GLOBAL_STATE->SYSTEM_MODULE.current_hashrate);
    else
    new_sensor_data.efficency = 0;

    if(health_module->fan_eft){
        snprintf(new_sensor_data.fan_mode, 20, "%"PRIu16"%%", health_module->fan_percent[0]);
    }else{
        strncpy(new_sensor_data.fan_mode, "AUTO", 20);
    }

    if(DEVICE_BC08 == GLOBAL_STATE->device_model || DEVICE_BC06 == GLOBAL_STATE->device_model)
        if(health_module->board_temperature[1] > new_sensor_data.temperature)
            new_sensor_data.temperature = health_module->board_temperature[1];

    refresh_sensor_data(new_sensor_data);
}

void refresh_hash_data(hash_data new_hash_data)
{
    /*refresh mining data and doge screen.*/
    _lock_acquire(&lvgl_api_lock);

    mining_screen_data *p_screen_data = &(volc_s3_display.m_mining_screen.m_mining_screen_data);
    p_screen_data->hashrate = new_hash_data.hashrate;
    p_screen_data->uptime_seconds = new_hash_data.uptime_seconds;
    strncpy(p_screen_data->str_best_diff, new_hash_data.str_best_diff,
        sizeof(p_screen_data->str_best_diff)/sizeof(p_screen_data->str_best_diff[0])
    );

    setting_data *p_setting_data = &(volc_s3_display.m_setting_screen.m_setting_data);
    p_setting_data->frequency = new_hash_data.frequency;
    strncpy(p_setting_data->str_best_diff, new_hash_data.str_best_diff, 
        sizeof(p_setting_data->str_best_diff)/sizeof(p_setting_data->str_best_diff[0]));
    p_setting_data->total_shares = new_hash_data.total_shares;
    if(new_hash_data.total_shares)
    p_setting_data->efficency = (float)new_hash_data.total_shares*100/(new_hash_data.total_shares+new_hash_data.shares_rejected);
    p_setting_data->hashrate = new_hash_data.hashrate;

    doge_screen_data *p_doge_data = &(volc_s3_display.m_doge_screen.m_doge_screen_data);
    p_doge_data->hashrate = new_hash_data.hashrate;
    p_doge_data->block_num = new_hash_data.block_num;
    
    _lock_release(&lvgl_api_lock);
}

void refresh_hash_data_from_system(GlobalState *GLOBAL_STATE)
{
    hash_data new_hash_data;

    volc_s3_display.m_dual_enable = GLOBAL_STATE->dual_enable;
    volc_s3_display.m_dual_ratio_a = GLOBAL_STATE->dual_ratio_a;

    new_hash_data.frequency = GLOBAL_STATE->asic_freqency;
    new_hash_data.hashrate = GLOBAL_STATE->SYSTEM_MODULE.current_hashrate;
    new_hash_data.uptime_seconds = (esp_timer_get_time() - GLOBAL_STATE->SYSTEM_MODULE.start_time) / 1000000;
    new_hash_data.total_shares = GLOBAL_STATE->SYSTEM_MODULE.shares_accepted;
    new_hash_data.shares_rejected = GLOBAL_STATE->SYSTEM_MODULE.shares_rejected;
    snprintf(new_hash_data.str_best_diff, sizeof(new_hash_data.str_best_diff), "%s", GLOBAL_STATE->SYSTEM_MODULE.best_session_diff_string);
    new_hash_data.block_num = GLOBAL_STATE->SYSTEM_MODULE.BLOCK_NUM;
    refresh_hash_data(new_hash_data);

    mining_screen_data *p_screen_data = &(volc_s3_display.m_mining_screen.m_mining_screen_data);
    snprintf(p_screen_data->model_str, sizeof(p_screen_data->model_str), "%s", GLOBAL_STATE->device_model_str);
}

void refresh_coin_data(coin_info new_coin_info)
{
    /*refresh doge_screen and global screen.*/
    _lock_acquire(&lvgl_api_lock);

    doge_screen_data *p_doge_data = &(volc_s3_display.m_doge_screen.m_doge_screen_data);
    strncpy(p_doge_data->ltc_price, new_coin_info.ltc_price, sizeof(p_doge_data->ltc_price)/sizeof(p_doge_data->ltc_price[0]));

    global_stats_data *p_global_stats = &(volc_s3_display.m_global_screen.m_global_stats_data);
    strncpy(p_global_stats->price, new_coin_info.ltc_price, sizeof(p_global_stats->price)/sizeof(p_global_stats->price[0]));
    strncpy(p_global_stats->global_diff, new_coin_info.global_ltc_diff, sizeof(p_global_stats->global_diff)/sizeof(p_global_stats->global_diff[0]));
    strncpy(p_global_stats->global_hashrate, new_coin_info.ltc_total_hashrate, sizeof(p_global_stats->global_hashrate)/sizeof(p_global_stats->global_hashrate[0]));
    snprintf(p_global_stats->ltc_blockheight, sizeof(p_global_stats->ltc_blockheight)/sizeof(p_global_stats->ltc_blockheight[0]), "%"PRIu32"", new_coin_info.ltc_block_height);
    strncpy(p_global_stats->halving_progress, new_coin_info.halving_progress, sizeof(p_global_stats->halving_progress)/sizeof(p_global_stats->halving_progress[0]));
    strncpy(p_global_stats->halving_blocks, new_coin_info.halving_blocks, sizeof(p_global_stats->halving_blocks)/sizeof(p_global_stats->halving_blocks[0]));

    _lock_release(&lvgl_api_lock);
}

void refresh_coin_data_from_system_test()
{
    coin_info new_coin_info = {
        .doge_price = "$0.178",
        .ltc_price = "$98.97",
        .doge_total_hashrate = "3.01", /*3.01 PH/s*/
        .ltc_total_hashrate = "2.70", /*2.70 PH/s */
        .doge_block_height = 5881803,
        .ltc_block_height = 2970107,
        .halving_blocks = "3360000",
        .halving_progress = "53.58%",
        .global_ltc_diff = "99.00M",
        .global_doge_diff = "36.430M"
    };

    refresh_coin_data(new_coin_info);
}

void refresh_network_from_system(char *stratum_url, uint16_t port)
{
    network_setting new_network_setting;

    strncpy(new_network_setting.pool_url, stratum_url, 20);
    new_network_setting.pool_port = port;
    NetWorkInfo netWork_Info = network_get_info();

    char * ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, CONFIG_ESP_WIFI_SSID);
    snprintf(new_network_setting.ssid, sizeof(new_network_setting.ssid), "%s", ssid);
    if(NULL != ssid)
        free(ssid);

    _lock_acquire(&lvgl_api_lock);

    mining_screen_data *p_screen_data = &(volc_s3_display.m_mining_screen.m_mining_screen_data);
    setting_data *p_setting_data = &(volc_s3_display.m_setting_screen.m_setting_data);

    strncpy(p_setting_data->pool_url, new_network_setting.pool_url, sizeof(p_setting_data->pool_url)/sizeof(p_setting_data->pool_url[0]));
    p_setting_data->pool_port = new_network_setting.pool_port;

    if(netWork_Info.eth_get_ip && netWork_Info.wifi_get_ip)
    {
        snprintf(p_screen_data->str_ip, sizeof(p_screen_data->str_ip), "%s", netWork_Info.eth_ip_str);
        snprintf(p_setting_data->str_ip, sizeof(p_setting_data->str_ip), "%s", netWork_Info.wifi_ip_str);
    }
    else if(netWork_Info.eth_get_ip)
    {
        snprintf(p_screen_data->str_ip, sizeof(p_screen_data->str_ip), "%s", netWork_Info.eth_ip_str);
        snprintf(p_setting_data->str_ip, sizeof(p_setting_data->str_ip), "%s", netWork_Info.eth_ip_str);
    }
    else if(netWork_Info.wifi_get_ip)
    {
        snprintf(p_screen_data->str_ip, sizeof(p_screen_data->str_ip), "%s", netWork_Info.wifi_ip_str);
        snprintf(p_setting_data->str_ip, sizeof(p_setting_data->str_ip), "%s", netWork_Info.wifi_ip_str);
    }

    _lock_release(&lvgl_api_lock);
}

void refresh_portal_data_from_system()
{
   wifi_config_t ap_config;
   esp_err_t err = esp_wifi_get_config(ESP_IF_WIFI_AP, &ap_config);
   if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP config, error: 0x%x", err);
        return;
   }
   ESP_LOGI(TAG, "SoftAP SSID: %.32s", ap_config.ap.ssid);
   
   _lock_acquire(&lvgl_api_lock);    
   snprintf(volc_s3_display.m_portal_screen.m_portal_screen_data.ssid, sizeof(volc_s3_display.m_portal_screen.m_portal_screen_data.ssid), "%.19s", (const char*)ap_config.ap.ssid);
   _lock_release(&lvgl_api_lock);
}

void refresh_ip_from_system(void)
{
    //network_setting new_network_setting;
    NetWorkInfo netWork_Info = network_get_info();

    //_lock_acquire(&lvgl_api_lock);
    mining_screen_data *p_screen_data = &(volc_s3_display.m_mining_screen.m_mining_screen_data);
    setting_data *p_setting_data = &(volc_s3_display.m_setting_screen.m_setting_data);

    if(netWork_Info.eth_get_ip && netWork_Info.wifi_get_ip)
    {
        snprintf(p_screen_data->str_ip, sizeof(p_screen_data->str_ip), "%s", netWork_Info.eth_ip_str);
        snprintf(p_setting_data->str_ip, sizeof(p_setting_data->str_ip), "%s", netWork_Info.wifi_ip_str);
    }
    else if(netWork_Info.eth_get_ip)
    {
        snprintf(p_screen_data->str_ip, sizeof(p_screen_data->str_ip), "%s", netWork_Info.eth_ip_str);
        snprintf(p_setting_data->str_ip, sizeof(p_setting_data->str_ip), "%s", netWork_Info.eth_ip_str);
    }
    else if(netWork_Info.wifi_get_ip)
    {
        snprintf(p_screen_data->str_ip, sizeof(p_screen_data->str_ip), "%s", netWork_Info.wifi_ip_str);
        snprintf(p_setting_data->str_ip, sizeof(p_setting_data->str_ip), "%s", netWork_Info.wifi_ip_str);
    }
    //_lock_release(&lvgl_api_lock);
}

bool ui_isActiveOverlay = false;

lv_obj_t *ui_imageOverlayContainer = NULL;
lv_obj_t *ui_errOverlayContainer = NULL;
lv_obj_t *ui_qrcodeOverlayContainer = NULL;

// Refresh screen values
void refreshScreen(void) {
    lv_timer_handler();
    lv_tick_inc(EXAMPLE_LVGL_REFRESH_SCREEN_INTERVAL);
}

void showImageOverlay(const lv_img_dsc_t *image)
{
    // Get the currently active screen
    lv_obj_t *current_screen = lv_scr_act();

    // Create a container for the overlay
    ui_imageOverlayContainer = lv_obj_create(current_screen);
    lv_obj_set_size(ui_imageOverlayContainer, LV_SIZE_CONTENT, LV_SIZE_CONTENT); // Size will fit image
    lv_obj_align(ui_imageOverlayContainer, LV_ALIGN_CENTER, 0, 0); // Center the overlay on the screen

    // Disable scrollbars for the container
    lv_obj_clear_flag(ui_imageOverlayContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Optional: make background transparent or keep style minimal
    lv_obj_set_style_bg_opa(ui_imageOverlayContainer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_imageOverlayContainer, 0, LV_PART_MAIN);

    // Create an image inside the container
    lv_obj_t *img = lv_img_create(ui_imageOverlayContainer);
    lv_img_set_src(img, image);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0); // Center the image inside the container
}

void hideImageOverlay(void)
{
    if (ui_imageOverlayContainer != NULL) {
        lv_obj_del(ui_imageOverlayContainer);
        ui_imageOverlayContainer = NULL;
    }
}

void showFoundBlockScreen(void) {
    // hide the overlay and free the memory in case it was open
    hideImageOverlay();

    // now show the (new) image overlay
    showImageOverlay(&found);
    ui_isActiveOverlay = true;
    refreshScreen();
}

void hideFoundBlockScreen(void) {
    // hide the overlay and free the memory
    hideImageOverlay();
    ui_isActiveOverlay = false;
}

// Function to show the overlay with an error message and custom colors
void showErrorOverlay(const char *error_message, uint32_t error_code)
{
    // Get the currently active screen
    lv_obj_t *current_screen = lv_scr_act();

    // Create a container for the overlay
    ui_errOverlayContainer = lv_obj_create(current_screen);
    lv_obj_set_size(ui_errOverlayContainer, 266, 120); // Set the size of the overlay box
    lv_obj_align(ui_errOverlayContainer, LV_ALIGN_CENTER, 0, 0); // Center the overlay on the screen

    // Disable scrollbars for the container
    lv_obj_clear_flag(ui_errOverlayContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Set background color and border style
    lv_obj_set_style_bg_color(ui_errOverlayContainer, lv_color_hex(0x0E4F00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_errOverlayContainer, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_errOverlayContainer, lv_color_hex(0xD10D25), LV_PART_MAIN | LV_STATE_DEFAULT);

    // Create the first label for the error message
    lv_obj_t *error_label = lv_label_create(ui_errOverlayContainer);
    lv_obj_set_width(error_label, LV_SIZE_CONTENT);  // Adjust width based on content
    lv_obj_set_height(error_label, LV_SIZE_CONTENT); // Adjust height based on content
    lv_obj_set_x(error_label, 0); // Center horizontally
    lv_obj_set_y(error_label, 0); // Align slightly below the top
    lv_obj_set_align(error_label, LV_ALIGN_CENTER); // Align center
    if(error_message)
    lv_label_set_text(error_label, error_message); // Set the error message text
    else
    lv_label_set_text(error_label, "unknown error"); // Set the error message text
    lv_obj_set_style_text_color(error_label, lv_color_hex(0xe60000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(error_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    //lv_obj_set_style_text_font(error_label, &lv_font_unscii_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(error_label, &lv_font_montserrat_28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(error_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    #if 0
    // Create the second label for the Guru Meditation Error
    lv_obj_t *code_label = lv_label_create(ui_errOverlayContainer);
    lv_obj_set_width(code_label, LV_SIZE_CONTENT);  // Adjust width based on content
    lv_obj_set_height(code_label, LV_SIZE_CONTENT); // Adjust height based on content
    lv_obj_set_x(code_label, 0); // Center horizontally
    lv_obj_set_y(code_label, 0); // Align slightly above the bottom
    lv_obj_set_align(code_label, LV_ALIGN_BOTTOM_MID); // Align bottom-middle

    // Format the error code message
    char error_code_message[64];
    snprintf(error_code_message, sizeof(error_code_message), "Ecode #%02X", (int) error_code);
    lv_label_set_text(code_label, error_code_message); // Set the error code message
    lv_obj_set_style_text_color(code_label, lv_color_hex(0xe60000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(code_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    //lv_obj_set_style_text_font(code_label, &lv_font_unscii_8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(code_label, &lv_font_montserrat_28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(code_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    #endif
}

void hideErrorOverlay(void)
{
    if (ui_errOverlayContainer != NULL) {
        lv_obj_del(ui_errOverlayContainer); // Delete the overlay object and its children
        ui_errOverlayContainer = NULL;     // Clear the pointer to avoid dangling references
    }
}

bool ui_isErrorScreen = false;
int ui_error_code = -1;

void showErrorScreen(const char *error_message, uint32_t error_code) 
{
    if(ui_isErrorScreen && (ui_error_code == error_code))
    {
        return;
    }

    // hide the overlay and free the memory in case it was open
    hideErrorOverlay();

    // now show the (new) error overlay
    showErrorOverlay(error_message, error_code);
    ui_isActiveOverlay = true;
    refreshScreen();

    ui_isErrorScreen = true;
    ui_error_code = error_code;
}

void hideErrorScreen(void) 
{
    if(!ui_isErrorScreen)
    {
        return;
    }
    // hide the overlay and free the memory
    hideErrorOverlay();
    ui_isActiveOverlay = false;

    ui_isErrorScreen = false;
    ui_error_code = -1;
}

void showQrOverlay(const char *str)
{
    // Get the currently active screen
    lv_obj_t *current_screen = lv_scr_act();

    // Create a container for the overlay
    ui_qrcodeOverlayContainer = lv_obj_create(current_screen);
    lv_obj_set_size(ui_qrcodeOverlayContainer, LV_SIZE_CONTENT, LV_SIZE_CONTENT); // Size will fit image
    lv_obj_align(ui_qrcodeOverlayContainer, LV_ALIGN_CENTER, 0, 0); // Center the overlay on the screen

    // Disable scrollbars for the container
    lv_obj_clear_flag(ui_qrcodeOverlayContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(ui_qrcodeOverlayContainer, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    // Optional: make background transparent or keep style minimal
    lv_obj_set_style_bg_opa(ui_qrcodeOverlayContainer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_qrcodeOverlayContainer, 0, LV_PART_MAIN);

    lv_obj_t *qr = lv_qrcode_create(ui_qrcodeOverlayContainer);

    lv_qrcode_set_size(qr,80);
    lv_qrcode_update(qr, str, strlen(str));
}

void hideQrOverlay(void)
{
    if (ui_qrcodeOverlayContainer != NULL) {
        lv_obj_del(ui_qrcodeOverlayContainer); // Delete the overlay object and its children
        ui_qrcodeOverlayContainer = NULL;     // Clear the pointer to avoid dangling references
    }
}

void showQrScreen(void) 
{
    // hide the overlay and free the memory in case it was open
    hideQrOverlay();

    // now show the (new) Qr overlay
    showQrOverlay(SYSTEM_get_sn());
    ui_isActiveOverlay = true;
    refreshScreen();
}

void hideQrScreen(void) 
{
    // hide the overlay and free the memory
    hideQrOverlay();
    ui_isActiveOverlay = false;
}

static int screen_flash = 0;
void displayFlashPro(void)
{
    static uint32_t time_last = 0;
    uint32_t now = esp_timer_get_time()/1000;

    if(screen_flash == 1)
    {
        if((now - time_last) > 490)
        {
            time_last = now;
            display_backlight_invert();
        }
    }
    else if(screen_flash == 2)
    {
        screen_flash = 0;
        display_on_off = 1;
        display_backlight_on();
    }
}

void displayFlashSet(int on)
{
    if(on)
        screen_flash = 1;
    else
        screen_flash = 2;
}
