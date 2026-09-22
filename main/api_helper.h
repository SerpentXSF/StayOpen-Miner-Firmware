#ifndef _API_HELPER_H
#define _API_HELPER_H

#include <stdbool.h>
#include "cJSON.h"
#include "global_state.h"

cJSON *get_network_info_json();
esp_err_t set_network_conf_json(cJSON *network_conf);

/*
 * True when a string is nothing but asterisks -- the settings page's way of
 * showing a stored secret it does not intend to change. Never store one.
 */
bool api_is_masked_secret(const char *s);




#endif