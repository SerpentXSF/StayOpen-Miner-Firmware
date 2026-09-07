cmake_minimum_required(VERSION 3.16)

set(SRCS
    "main.c"
    "nvs_config.c"
    "nvs_device.c"
    "work_queue.c"
    "system.c"
    "device.c"
    "api_helper.c"
    "statistic.c"
    "history.c"
    "network.c"
    "./http_server/http_server.c"
    "./http_server/api_auth.c"
    "./http_server/theme_api.c"
    "./self_test/self_test.c"
    "./tasks/stratum_task.c"
    "./tasks/stratum_poolb_task.c"
    "./tasks/create_jobs_task.c"
    "./tasks/asic_result_task.c"
    "./tasks/asic_task.c"
    "./tasks/health_maintennance.c"
    "./tasks/protocol_task.c"
    "./tasks/socket_api_task.c"
    "./tasks/influx_task.c"
    "./tasks/hashrate_monitor_task.c"
    "./tasks/ping_task.c"
    "./tasks/http_task.c"
)

set(INCLUDE_DIRS 
    "."
    "tasks"
    "self_test"
    "http_server"
    "../components/connect/include"
    "../components/dns_server/include"
    "../components/stratum/include"
    "../components/asic/include"
    "../components/bc_hal/include"
    "../components/protocol/include"
    "../components/influx/include"
)

set(PRIV_REQUIRES
    "app_update"
    "driver"
    "esp_app_format"
    "esp_event"
    "esp_http_server"
    "esp_http_client"
    "esp_netif"
    "esp_timer"
    "esp_wifi"
    "json"
    "nvs_flash"
    "spiffs"
    "vfs"
    "esp_driver_i2c"
    "spi_flash"
    "protocol"
    "esp_lcd"
    "esp-tls"
    "esp_eth"
    "bc_hal"
    "dual_pool"
# EMBED_FILES "http_server/recovery_page.html"
)

set(EMBED_FILES 
    "http_server/recovery_page.html"
)

# Every supported device module builds the same display stack. These were
# four near-identical copies of this block; they differed only in embedding
# secure/flash_encryption_key.bin, which was never part of the source
# release and is no longer referenced (see docs/OTA-FORMAT.md).
set(DISPLAY_MODULES "lotto" "DC02" "DC04" "BC01" "BC02" "BC04" "BC06" "BC08")

if("${CONFIG_DEVICE_MODULE}" IN_LIST DISPLAY_MODULES)
    message(STATUS "Device module: ${CONFIG_DEVICE_MODULE} (display stack enabled)")

    list(APPEND SRCS
        "./displays/images/ui_font_DigitalNumbers16.c"
        "./displays/images/ui_font_DigitalNumbers28.c"
        "./displays/images/ui_font_OpenSansBold13.c"
        "./displays/images/ui_font_OpenSansBold14.c"
        "./displays/images/ui_font_OpenSansBold24.c"
        "./displays/images/ui_font_OpenSansBold45.c"
        "./displays/images/ui_font_vt323_21.c"
        "./displays/images/ui_font_vt323_35.c"
        "./displays/lvgl_porting.c"
        "./displays/lv_input.c"
        "./displays/lvgl_screen.c"
        "./displays/images/themes/themes.c"
    )

    list(APPEND INCLUDE_DIRS
        "displays"
        "displays/images"
        "displays/images/themes"
    )

    list(APPEND PRIV_REQUIRES
        "lvgl"
        "esp_lcd"
    )

    include_directories("./displays")

    add_definitions(-DLV_LVGL_H_INCLUDE_SIMPLE)
else()
    message(STATUS "Device module: ${CONFIG_DEVICE_MODULE} (no display stack)")
endif()


idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    PRIV_REQUIRES ${PRIV_REQUIRES}
    EMBED_FILES   ${EMBED_FILES}
)


set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/http_server/axe-os")

if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    message(STATUS "Running on GitHub Actions. Web ui will be prebuilt.")

    spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist/axe-os FLASH_IN_PROJECT)
else()
    find_program(NPM_EXECUTABLE npm)
    if(NOT NPM_EXECUTABLE AND NOT EXISTS ${WEB_SRC_DIR}/dist)
        message(FATAL_ERROR "npm is not found! Please install it to proceed.")
    endif()

    ExternalProject_Add(
        web_ui_dist
        PREFIX ${CMAKE_BINARY_DIR}/web_ui_dist
        SOURCE_DIR ${WEB_SRC_DIR}
        DOWNLOAD_COMMAND ""
        CONFIGURE_COMMAND
            ${NPM_EXECUTABLE}  i
        USES_TERMINAL_BUILD true
        BUILD_COMMAND
            ${NPM_EXECUTABLE} run build
        INSTALL_COMMAND ""
        BUILD_ALWAYS OFF
        BUILD_IN_SOURCE TRUE
        BUILD_BYPRODUCTS
            "${WEB_SRC_DIR}/dist/index.html"
    )

    add_dependencies(${COMPONENT_LIB} web_ui_dist)

    spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist/ FLASH_IN_PROJECT DEPENDS web_ui_dist)
endif()
