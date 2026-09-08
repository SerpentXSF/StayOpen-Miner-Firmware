#include <string.h>
#include "esp_log.h"

#include "bm1370.h"

#ifdef CONFIG_STAYOPEN_ASIC_LT0051
#include "lt0051.h"
#else
/*
 * LT0051 support is compiled out, and with it the only caller of
 * components/a/liba.a -- a prebuilt archive this project has no source for.
 *
 * The dispatch arms below are left in place rather than deleted: they are the
 * record of which device models this driver would serve, and removing them
 * would make the switches read as though those boards were never considered.
 * They are unreachable on a BC board in any case, since device_model comes
 * from NVS and no BC board reports an LT0051 part. What they must not do is
 * reference the driver, because the linker resolves references whether or not
 * the call can ever be made -- which is exactly how 82 sections of that
 * archive ended up at live flash addresses in images documented as blob-free.
 */
#include "lt0051.h"
#define LT0051_init_by_chain(state, chain, freq, count)     ((void)(state), (void)(chain), (void)(freq), (void)(count), ESP_FAIL)
#define LT0051_set_max_baud_by_chain(state, chain)     ((void)(state), (void)(chain), 0)
#define LT0051_process_work(state, chain)     ((void)(state), (void)(chain), (task_result *)NULL)
#define LT0051_send_work(state, job, chain, workid)     ((void)(state), (void)(job), (void)(chain), (void)(workid))
#define LT0051_send_hash_frequency(state, freq)     ((void)(state), (void)(freq), false)
#endif
#include "asic.h"
#include "frequency_transition_bmXX.h"

static const char *TAG = "asic";

esp_err_t ASIC_detect(GlobalState * GLOBAL_STATE){
    esp_err_t ret = ESP_FAIL;
    uint8_t detetecd_asic = 0;

    switch (GLOBAL_STATE->device_model) 
    {
        case DEVICE_LOTTO:
            GLOBAL_STATE->tmmusk = VOLCMINER_LOTTO_TM_DEFINE;
            
            ret = SERIAL_init(0);

            if(ESP_OK == ret){
                detetecd_asic = ASIC_init(GLOBAL_STATE);
                ESP_LOGI(TAG, "Model LOTTO, Detect %"PRIu8" asic.", detetecd_asic);
            }

            if(VOLCMINER_LOTTO_ASIC_COUNT == detetecd_asic){
                //SERIAL_set_baud(ASIC_set_max_baud(&GLOBAL_STATE));
                SERIAL_clear_buffer(0);
                GLOBAL_STATE->ASIC_initalized = true;
                ret = ESP_OK;
            }else{
                ret = ESP_FAIL;
            }
            break;

        case DEVICE_DC02:
            GLOBAL_STATE->tmmusk = DC02_LOTTO_TM_DEFINE;
            
            ret = SERIAL_init(0);

            if(ESP_OK == ret){
                detetecd_asic = ASIC_init(GLOBAL_STATE);
                ESP_LOGI(TAG, "Model DC02, Detect %"PRIu8" asic.", detetecd_asic);
            }

            if(DC02_LOTTO_ASIC_COUNT == detetecd_asic){
                //SERIAL_set_baud(ASIC_set_max_baud(&GLOBAL_STATE));
                SERIAL_clear_buffer(0);
                GLOBAL_STATE->ASIC_initalized = true;
                ret = ESP_OK;
            }else{
                ret = ESP_FAIL;
            }
            break;

        case DEVICE_DC04:
            GLOBAL_STATE->tmmusk = DC04_LOTTO_TM_DEFINE;
            
            ret = SERIAL_init(0);

            if(ESP_OK == ret){
                detetecd_asic = ASIC_init(GLOBAL_STATE);
                ESP_LOGI(TAG, "Model DC04, Detect %"PRIu8" asic.", detetecd_asic);
            }

            if(DC04_LOTTO_ASIC_COUNT == detetecd_asic){
                //SERIAL_set_baud(ASIC_set_max_baud(&GLOBAL_STATE));
                SERIAL_clear_buffer(0);
                GLOBAL_STATE->ASIC_initalized = true;
                ret = ESP_OK;
            }else{
                ret = ESP_FAIL;
            }
            break;

		case DEVICE_DC06:
            GLOBAL_STATE->tmmusk = DC06_LOTTO_TM_DEFINE;
            
            ret = SERIAL_init(0);

            if(ESP_OK == ret){
                detetecd_asic = ASIC_init(GLOBAL_STATE);
                ESP_LOGI(TAG, "Model DC06, Detect %"PRIu8" asic.", detetecd_asic);
            }

            if(DC06_LOTTO_ASIC_COUNT == detetecd_asic){
                //SERIAL_set_baud(ASIC_set_max_baud(&GLOBAL_STATE));
                SERIAL_clear_buffer(0);
                GLOBAL_STATE->ASIC_initalized = true;
                ret = ESP_OK;
            }else{
                ret = ESP_FAIL;
            }
            break;

        /* Every BC board runs the same BM1370 bring-up; only the expected
         * ASIC count differs, and that comes from ASIC_get_asic_count().
         * The vendor tree carried this body three times, once per model,
         * and had no BC01 or BC02 case at all -- so those boards fell
         * through to default and never initialised, despite nvs_device.c
         * already configuring them. */
        case DEVICE_BC01:
        case DEVICE_BC02:
        case DEVICE_BC04:
        case DEVICE_BC06:
        case DEVICE_BC08: {
            uint8_t expected_asic = ASIC_get_asic_count(GLOBAL_STATE);

            GLOBAL_STATE->tmmusk = 0;

            ret = SERIAL_init(0);

            if(ESP_OK == ret){
                for(int i=0;i<10;i++)
                {
                    detetecd_asic = ASIC_init(GLOBAL_STATE);
                    ESP_LOGI(TAG, "Model %s, Detect %"PRIu8" asic.",
                             GLOBAL_STATE->device_model_str, detetecd_asic);
                    SERIAL_clear_buffer(0);
                    if(expected_asic == detetecd_asic)
                    {
                        break;
                    }
                }
            }

            if(expected_asic == detetecd_asic){
                SERIAL_set_baud(0,ASIC_set_max_baud(GLOBAL_STATE));
                SERIAL_clear_buffer(0);
                GLOBAL_STATE->ASIC_initalized = true;
                GLOBAL_STATE->chain_pluged[0] = true;
                ret = ESP_OK;
            }else{
                ret = ESP_FAIL;
            }
            break;
        }

        default:
            break;
    }

    return ret;
}

uint8_t ASIC_init(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) 
    {
        case DEVICE_LOTTO:
            if(ESP_OK == LT0051_init_by_chain(GLOBAL_STATE, 0, VOLCMINER_LOTTO_INIT_FREQUENCY, VOLCMINER_LOTTO_ASIC_COUNT))
                return VOLCMINER_LOTTO_ASIC_COUNT;
            else
                return 0;

        case DEVICE_DC02:
            if(ESP_OK == LT0051_init_by_chain(GLOBAL_STATE, 0, DC02_LOTTO_INIT_FREQUENCY, DC02_LOTTO_ASIC_COUNT))
                return DC02_LOTTO_ASIC_COUNT;
            else
                return GLOBAL_STATE->asic_count[0];

        case DEVICE_DC04:
            if(ESP_OK == LT0051_init_by_chain(GLOBAL_STATE, 0, DC04_LOTTO_INIT_FREQUENCY, DC04_LOTTO_ASIC_COUNT))
                return DC04_LOTTO_ASIC_COUNT;
            else
                return GLOBAL_STATE->asic_count[0];

        case DEVICE_DC06:
            if(ESP_OK == LT0051_init_by_chain(GLOBAL_STATE, 0, DC06_LOTTO_INIT_FREQUENCY, DC06_LOTTO_ASIC_COUNT))
                return DC06_LOTTO_ASIC_COUNT;
            else
                return GLOBAL_STATE->asic_count[0];

        case DEVICE_BC01:
        case DEVICE_BC02:
        case DEVICE_BC04:
        case DEVICE_BC06:
        case DEVICE_BC08:
            GLOBAL_STATE->asic_count[0] = BM1370_init(GLOBAL_STATE->asic_freqency,
                                                      ASIC_get_asic_count(GLOBAL_STATE),
                                                      GLOBAL_STATE->asic_difficulty);
            return GLOBAL_STATE->asic_count[0];

        default:
            break;
    }
    return 0;
}

// .set_max_baud_fn = LT0051_set_max_baud,
int ASIC_set_max_baud(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) 
    {
        case DEVICE_LOTTO:
            return LT0051_set_max_baud_by_chain(GLOBAL_STATE, 0);
        case DEVICE_DC02:
            return LT0051_set_max_baud_by_chain(GLOBAL_STATE, 0);
        case DEVICE_DC04:
            return LT0051_set_max_baud_by_chain(GLOBAL_STATE, 0);
		case DEVICE_DC06:
            return LT0051_set_max_baud_by_chain(GLOBAL_STATE, 0);
        case DEVICE_BC01:
        case DEVICE_BC02:
        case DEVICE_BC04:
        case DEVICE_BC06:
        case DEVICE_BC08:
            return BM1370_set_max_baud();
        default:
            return 0;
    }
}

/*for http_server.*/
uint8_t ASIC_get_asic_count(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) 
    {
        case DEVICE_LOTTO:
            return VOLCMINER_LOTTO_ASIC_COUNT;
        case DEVICE_DC02:
            return DC02_LOTTO_ASIC_COUNT;
        case DEVICE_DC04:
            return DC04_LOTTO_ASIC_COUNT;
		case DEVICE_DC06:
			return DC06_LOTTO_ASIC_COUNT; 
        case DEVICE_BC01:
            return BC01_LOTTO_ASIC_COUNT;
        case DEVICE_BC02:
            return BC02_LOTTO_ASIC_COUNT;
        case DEVICE_BC04:
            return BC04_LOTTO_ASIC_COUNT;
        case DEVICE_BC08:
            return BC08_LOTTO_ASIC_COUNT;
        case DEVICE_BC06:
            /* Was BC08_LOTTO_ASIC_COUNT, which reported 8 ASICs on a
             * 6-ASIC board and skewed every per-chip figure derived
             * from it. */
            return BC06_LOTTO_ASIC_COUNT;
        default:
            return 0;
    }

    return 0;
}

/*for http_server.*/
uint16_t ASIC_get_small_core_count(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) 
    {
        case DEVICE_LOTTO:
            return 132;
        case DEVICE_DC02:
            return 132;
        case DEVICE_DC04:
            return 132;  
        case DEVICE_DC06:
            return 132;		
        case DEVICE_BC01:
        case DEVICE_BC02:
        case DEVICE_BC04:
        case DEVICE_BC06:
        case DEVICE_BC08:
            return 2040;
        default:
            return 0;
    }

    return 0;
}

// .receive_result_fn = LT0051_process_work,
task_result * ASIC_process_work(GlobalState * GLOBAL_STATE, uint32_t chain_num) {
    switch (GLOBAL_STATE->device_model) 
    {
        case DEVICE_LOTTO:
            return LT0051_process_work(GLOBAL_STATE, chain_num);
        case DEVICE_DC02:
            return LT0051_process_work(GLOBAL_STATE, chain_num);
        case DEVICE_DC04:
            return LT0051_process_work(GLOBAL_STATE, chain_num);  
        case DEVICE_DC06:
            return LT0051_process_work(GLOBAL_STATE, chain_num); 
        case DEVICE_BC01:
        case DEVICE_BC02:
        case DEVICE_BC04:
        case DEVICE_BC06:
        case DEVICE_BC08:
            return BM1370_process_work(GLOBAL_STATE);
        default:
			break;
    }

    return NULL;
}

// .set_difficulty_mask_fn = LT0051_set_job_difficulty_mask,
void ASIC_set_job_difficulty_mask(GlobalState * GLOBAL_STATE, uint8_t mask) {
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_LOTTO:
            break;
        case DEVICE_DC02:
            break;
        case DEVICE_DC04:
            break;  
        case DEVICE_DC06:
            break;			
        default:
            break;
    }
}

// .send_work_fn = LT0051_send_work,
void ASIC_send_work(GlobalState * GLOBAL_STATE, void * next_job, uint32_t chain_num, uint8_t workid) {
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_LOTTO:
            LT0051_send_work(GLOBAL_STATE, next_job, chain_num, workid);
            break;
        case DEVICE_DC02:
            LT0051_send_work(GLOBAL_STATE, next_job, chain_num, workid);
            break;
        case DEVICE_DC04:
            LT0051_send_work(GLOBAL_STATE, next_job, chain_num, workid);
            break;            
		case DEVICE_DC06:
			LT0051_send_work(GLOBAL_STATE, next_job, chain_num, workid);
			break;
        case DEVICE_BC04:
			BM1370_send_work(GLOBAL_STATE, next_job);
			break;
        case DEVICE_BC08:
			BM1370_send_work(GLOBAL_STATE, next_job);
			break;
        case DEVICE_BC06:
        case DEVICE_BC01:
        case DEVICE_BC02:
			BM1370_send_work(GLOBAL_STATE, next_job);
			break;            
            //LT0051_send_work_by_chip(GLOBAL_STATE, next_job, chain_num, workid, ASIC_get_asic_count(GLOBAL_STATE));
            break;
        default:
    }
}

// .set_version_mask = LT0051_set_version_mask
void ASIC_set_version_mask(GlobalState * GLOBAL_STATE, uint32_t mask) {
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_LOTTO:
            //do_nothing.
            break;
        case DEVICE_DC02:
            //do_nothing.
            break;
        case DEVICE_DC04:
            //do_nothing.
            break;
        case DEVICE_DC06:
            //do_nothing.
            break;			
        case DEVICE_BC04:
            BM1370_set_version_mask(mask);
            break;
        case DEVICE_BC08:
            BM1370_set_version_mask(mask);
            break;
        case DEVICE_BC06:
        case DEVICE_BC01:
        case DEVICE_BC02:
            BM1370_set_version_mask(mask);
            break;            
        default:
            return;
    }
}

bool ASIC_set_frequency(GlobalState * GLOBAL_STATE, float target_frequency) {
    ESP_LOGI(TAG, "Setting ASIC frequency to %.2f MHz", target_frequency);
    bool success = false;

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_LOTTO:
            success = LT0051_send_hash_frequency(GLOBAL_STATE, target_frequency);
            break;
        case DEVICE_DC02:
            success = LT0051_send_hash_frequency(GLOBAL_STATE, target_frequency);
            break;
        case DEVICE_DC04:
            success = LT0051_send_hash_frequency(GLOBAL_STATE, target_frequency);
            break; 
		case DEVICE_DC06:
			success = LT0051_send_hash_frequency(GLOBAL_STATE, target_frequency);
			break;
		case DEVICE_BC04:
			success = true;
            do_frequency_transition(target_frequency, BM1370_send_hash_frequency);
			break;
		case DEVICE_BC08:
			success = true;
            do_frequency_transition(target_frequency, BM1370_send_hash_frequency);
			break;
		case DEVICE_BC06:
        case DEVICE_BC01:
        case DEVICE_BC02:
			success = true;
            do_frequency_transition(target_frequency, BM1370_send_hash_frequency);
			break;
        default:
            ESP_LOGE(TAG, "Unknown ASIC model, cannot set frequency");
            success = false;
            break;
    }

    if (success) {
        ESP_LOGI(TAG, "Successfully transitioned to new ASIC frequency: %.2f MHz", target_frequency);
    } else {
        ESP_LOGE(TAG, "Failed to transition to new ASIC frequency: %.2f MHz", target_frequency);
    }

    return success;
}

esp_err_t ASIC_set_device_model(GlobalState * GLOBAL_STATE) {

    if (GLOBAL_STATE->device_model_str == NULL) {
        ESP_LOGE(TAG, "No device model string found");
        return ESP_FAIL;
    }

    return ESP_OK;
}

int ASIC_switch_by_chain(GlobalState *state, uint32_t chain_num)
{
    return switch_by_chain(state, chain_num);
}

void ASIC_read_registers(GlobalState * GLOBAL_STATE)
{
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_BC04:
            BM1370_read_registers(GLOBAL_STATE);
            break;
        case DEVICE_BC08:
            BM1370_read_registers(GLOBAL_STATE);
            break;
        case DEVICE_BC06:
        case DEVICE_BC01:
        case DEVICE_BC02:
            BM1370_read_registers(GLOBAL_STATE);
            break;             
        default:
            break;
    }
}

