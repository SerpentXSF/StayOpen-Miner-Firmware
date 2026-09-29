#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "serial.h"
#include "lt0051.h"
#include "asic.h"
#include "asic_abstration.h"
#include "utils.h"

static const char *TAG = "lt0051";

#undef LT0051_DEBUG

void config_all_top_reg(bool is_all, uint8_t chip_addr, 
                    uint8_t reg_addr, uint32_t reg_value, bool should_check)
{
    for(int i = 0; i < MAX_CHAIN_NUM; i++)
    {
        set_top_reg(i, is_all, chip_addr, reg_addr, reg_value, should_check);
    }
}

void config_all_core_reg(bool is_all, uint8_t chip_addr, uint8_t is_all_core, uint8_t core_id,
                    uint8_t reg_addr, uint32_t reg_value, bool should_check)
{
    for(int i = 0; i < MAX_CHAIN_NUM; i++)
    {
        set_core_reg(i, is_all, chip_addr, is_all_core, core_id, reg_addr, reg_value, should_check);
    }
}

/**
 * @brief check all the reg of top.
 *        Should be used after the asic-detection of chain.
 * 
 * @return  1. negative_value, read_value != reg_value.
 *          2. 0, read nothing from the chain.
 *          3. positive vaule, the correct times of read_value == reg_value
 */
int check_all_top_reg(GlobalState *state, bool is_all, uint8_t chip_addr, uint8_t reg_addr, uint32_t reg_value)
{
    int min_result_chain = 1000, tmp_result_chain = 0;

    for(int i = 0; i < MAX_CHAIN_NUM; i++){
        if(true == state->chain_pluged[i]){
            tmp_result_chain = check_top_reg(i, is_all, chip_addr, reg_addr, reg_value);

            if(tmp_result_chain < min_result_chain){
                min_result_chain = tmp_result_chain;
            }          
        }
    }

    return min_result_chain;
}


/**
 * @brief check all the reg of top.
 *
 * @return  1. negative_value, read_value != reg_value.
 *          2. 0, read nothing from the chain.
 *          3. positive vaule, the correct times of read_value == reg_value
 */
int check_all_core_reg(GlobalState *state, bool is_all, uint8_t chip_addr, uint8_t is_all_core, uint8_t core_id,
                        uint8_t reg_addr, uint32_t reg_value)
{
    int ret = 0;

    /*TODO*/

    return ret;
}

/*register releated operation.*/
bool check_all_pll_status(GlobalState *state, uint8_t expected_asic_count)
{
    bool ret = false;
    int asic_num = 0;
    uint8_t chip_addr = 0x0;
    uint32_t pll_check_value = 0x4bf;
    uint8_t detected_chain = 0;

    for(int i = 0; i < MAX_CHAIN_NUM; i++){
        asic_num = check_top_reg(i, true, chip_addr, REG_PLL_STATUS, pll_check_value);
        //asic_num = check_top_reg(i, true, chip_addr, REG_CHIP_ADDRESS, 0);

        if(asic_num > 0)
            ESP_LOGI(TAG, "chain %d,  asic_num %d", i, asic_num);
        else if(asic_num < 0)
            ESP_LOGI(TAG, "chain %d is not locked,  asic_num %d", i, -asic_num);
        else if(asic_num == 0)
            ESP_LOGI(TAG, "chain %d detect no asic.", i);

        if(expected_asic_count == asic_num){
            /*mark the chain*/
            state->chain_pluged[i] = true;
            state->asic_count[i] = asic_num;
            detected_chain += 1;            
        }else if (asic_num < 0){
            ret = false;
            goto exit;
        }
    }

    /*pass the pll check if detect 1~2 chains.*/
    if(detected_chain > 0)
        ret = true;
    
exit:
    return ret;
}

bool check_pll_status_by_chain(GlobalState *state, int chain_num, uint8_t expected_asic_count)
{
    bool ret = false;
    int asic_num = 0;
    uint8_t chip_addr = 0x0;
    uint32_t pll_check_value = 0x4bf;
    uint8_t detected_chain = 0;

    asic_num = check_top_reg(chain_num, true, chip_addr, REG_PLL_STATUS, pll_check_value);
    //asic_num = check_top_reg(i, true, chip_addr, REG_CHIP_ADDRESS, 0);
    if(asic_num > 0)
        ESP_LOGI(TAG, "chain %d,  asic_num %d", chain_num, asic_num);
    else if(asic_num < 0)
        ESP_LOGI(TAG, "chain %d is not locked,  asic_num %d", chain_num, -asic_num);
    else if(asic_num == 0)
        ESP_LOGI(TAG, "chain %d detect no asic.", chain_num);

    if(expected_asic_count == asic_num){
        /*mark the chain*/
        state->chain_pluged[chain_num] = true;
        state->asic_count[chain_num] = asic_num;
        detected_chain += 1;            
    }else if (asic_num < 0){
        ret = false;
        goto exit;
    }
    else if(asic_num > 0)
    {
        state->asic_count[chain_num] = asic_num;
    }

    /*pass the pll check if detect 1~2 chains.*/
    if(detected_chain > 0)
        ret = true;
    
exit:
    return ret;
}

bool config_all_asic_address(GlobalState *state, uint8_t asic_count)
{
    bool ret = false;

    uint8_t addr_interval = 0x100 / asic_count;

    if(1 == asic_count){
        //the default address is ok.
        ret = true;
        goto exit;
    }

    /*chain inactive*/
    for(int i = 0; i < MAX_CHAIN_NUM; i++){
        if(state->chain_pluged[i]){
            ESP_LOGI(TAG, "Chain %d inactive.", i);
            chain_inactive(i);
        }
    }

    /*set address.*/
    for(int i = 0; i < MAX_CHAIN_NUM; i++){
        if(state->chain_pluged[i]){
            ESP_LOGI(TAG, "Chain %d start to set address, addr interval is %"PRIu8"", i, addr_interval);

            for(int j = 0; j < asic_count; j++){
                set_address(i, j*addr_interval);
            }
        }
    }

exit:
    return ret;
}

bool config_asic_address_by_chain(GlobalState *state, int chain_num, uint8_t asic_count)
{
    bool ret = false;
    uint8_t addr_interval = 0x100 / asic_count;

    if(1 == asic_count){
        //the default address is ok.
        ret = true;
        goto exit;
    }

    /*chain inactive*/
    if(state->chain_pluged[chain_num]){
        ESP_LOGI(TAG, "Chain %d inactive.", chain_num);
        chain_inactive(chain_num);
    }

    /*set address.*/
    if(state->chain_pluged[chain_num]){
        ESP_LOGI(TAG, "Chain %d start to set address, addr interval is %"PRIu8"", chain_num, addr_interval);

        for(int j = 0; j < asic_count; j++){
            set_address(chain_num, j*addr_interval);
            check_top_reg(chain_num, false, j*addr_interval, REG_CHIP_ADDRESS, j*addr_interval);
        }
    }
exit:
    return ret;
}


uint8_t getDivQ(uint8_t divq){
	if(divq==0)
		return 2;
	else
		return 4*divq;
}

uint16_t getRange(float postDRef){
	if(postDRef>=7 && postDRef<=11){
		return 1;
	}else if(postDRef>=11 && postDRef<=18){
		return 2;
	}else if(postDRef>=18 && postDRef<=30){
		return 3;
	}else if(postDRef>=30 && postDRef<=50){
		return 4;
	}else if(postDRef>=50 && postDRef<=80){
		return 5;
	}else if(postDRef>=80 && postDRef<=130){
		return 6;
	}else if(postDRef>=130 && postDRef<=200){
		return 7;
	}else{
		ESP_LOGW(TAG,"getRange Fatal Error !!!!");
		return 1;
	}
}

pll_config_data calc_pll_param(uint16_t freq, float *ret_f_freq)
{
    pll_config_data param;
    float post_ref_min=7.0;
	float post_ref_max=200.0;
	uint16_t divr=0,ret_divr=0;

	uint32_t vco_min=4000;
	uint32_t vco_max=8000;
	uint16_t divf=0,ret_divf=0;

	uint16_t divq=1,ret_divq=1,ret_range=1;
	float tfreq=0.0,tmp=0.0;

	for(divr=MIN_DIVR;divr<=MAX_DIVR;divr++){
		//BASE_CLK_M:50
		if(BASE_CLK_M/(divr+1)<post_ref_min || BASE_CLK_M/(divr+1)>post_ref_max){
			continue;
		}

		for(divf=MIN_DIVF;divf<=MAX_DIVF;divf++){
			if(BASE_CLK_M/(divr+1)*2*(divf+1)<vco_min || BASE_CLK_M/(divr+1)*2*(divf+1)>vco_max){
				continue;
			}

			for(divq=0x0;divq<=0xFF;divq++){
				tmp = BASE_CLK_M/(divr+1)*2*(divf+1)/getDivQ(divq);
				if(tmp<=freq && freq-tmp<freq-tfreq ){
					tfreq = tmp;
					ret_divr = divr;
					ret_divf = divf;
					ret_divq = divq;
				}	
			}
		}
	}
    
    //ESP_LOGI(TAG, "found freq:%f", tfreq);
    ret_range = getRange(BASE_CLK_M/(ret_divr+1));

    //return the real frequency.
    *ret_f_freq = BASE_CLK_M/(ret_divr+1)*2*(ret_divf+1)/getDivQ(ret_divq);

    param.pll_resetn = 0;
    param.pll_bypass = 0;
    param.pll_range = ret_range;
    param.pll_divq = ret_divq;
    param.pll_divf = ret_divf;
    param.pll_divr = ret_divr;
    param.rsv = 0;

    return param;
}

int config_asic_freq_by_chain(GlobalState *state, int chain_num, uint16_t freq, uint8_t pll_index)
{
    int ret = 0;
    uint8_t pll_reg_addr = 0;

    if(0 == pll_index){
        pll_reg_addr = REG_PLL0_CONFIG;
    }else if(1 == pll_index){
        pll_reg_addr = REG_PLL1_CONFIG;
    }else{
        ESP_LOGW(TAG, "pll_index %u Error.", pll_reg_addr);
        ret = -1;
        goto exit;
    }

    if(state->chain_pluged[chain_num]){
        pll_config_data param = calc_pll_param(freq, state->real_freq + chain_num);
        //ESP_LOGD(TAG, "chain %d, PLL param: %"PRIx32"", chain_num, *(uint32_t *)(&param));
        //set bypass mode:
        param.pll_resetn = 1;
        param.pll_bypass = 1;
        set_top_reg(chain_num, true, 0, pll_reg_addr, *(uint32_t *)(&param), false);
        //busy_wait_ms(5);
        vTaskDelay(pdMS_TO_TICKS(5));
        //set freq:
        param.pll_resetn = 1;
        param.pll_bypass = 1;
        set_top_reg(chain_num, true, 0, pll_reg_addr, *(uint32_t *)(&param), false);
        //busy_wait_ms(5);
        vTaskDelay(pdMS_TO_TICKS(5));
        //disable bypass mode:
        param.pll_resetn = 1;
        param.pll_bypass = 0;
        set_top_reg(chain_num, true, 0, pll_reg_addr, *(uint32_t *)(&param), false);
        //busy_wait_ms(20);
        vTaskDelay(pdMS_TO_TICKS(20));
        //ESP_LOGD(TAG, "chain: %d, freq: %"PRIu16", f_divr: %u, f_divf: %u, f_divq: %u ",
        //   chain_num, freq, param.pll_divr, param.pll_divf, param.pll_divq);

        /*TOCHECK: check the pll status.*/
        //get_top_reg(chain_num, true, 0, REG_PLL_STATUS, 0);
        //busy_wait_ms(10);

        //swith the hash clock:
        set_top_reg(chain_num, true, 0, REG_HASH_CLK_CTRL, pll_index, false);
        //busy_wait_ms(15);
        vTaskDelay(pdMS_TO_TICKS(15));

        ESP_LOGI(TAG, "chain: %d, freq: %"PRIu16", pll: %"PRIu8"", chain_num, freq, pll_index);
    }

exit:
    return ret;
}

int config_all_asic_freq(GlobalState *state, uint16_t freq, uint8_t pll_index)
{
    int ret = 0;

    for(int chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++)
    {
        if(state->chain_pluged[chain_num]){
            ret = config_asic_freq_by_chain(state, chain_num, freq, pll_index);
            if(ret < 0){
                break;
            }
        }
    }

    return ret;
}

/*
ret:
1, completed.
0, successed
-1, fail
*/

static uint8_t pll0pll1[MAX_CHAIN_NUM] = {0, 0};
bool b_switch_done[MAX_CHAIN_NUM] = {false, false};

int switch_by_chain(GlobalState *state, uint32_t chain_num)
{
    int ret = 0;

    switch (state->device_model)
    {
        case DEVICE_LOTTO:
            if(b_switch_done[chain_num])
                return 1;
            if(state->real_freq[chain_num] <= (float)(state->asic_freqency - VOLCMINER_LOTTO_SWITCH_STEP)){
                ret = config_asic_freq_by_chain(state, chain_num, (uint16_t)(state->real_freq[chain_num])+VOLCMINER_LOTTO_SWITCH_STEP, pll0pll1[chain_num]);
            }else{
                ret = config_asic_freq_by_chain(state, chain_num, state->asic_freqency, pll0pll1[chain_num]);
                b_switch_done[chain_num] = true;
            }

            pll0pll1[chain_num] = (pll0pll1[chain_num]+1)%2;            
            break;

        case DEVICE_DC02:
        case DEVICE_DC04:
		case DEVICE_DC06:
            if(b_switch_done[chain_num])
                return 1;
            if(state->real_freq[chain_num] <= (float)(state->asic_freqency - DC02_LOTTO_SWITCH_STEP)){
                ret = config_asic_freq_by_chain(state, chain_num, (uint16_t)(state->real_freq[chain_num])+DC02_LOTTO_SWITCH_STEP, pll0pll1[chain_num]);
            }else{
                ret = config_asic_freq_by_chain(state, chain_num, state->asic_freqency, pll0pll1[chain_num]);
                b_switch_done[chain_num] = true;
            }

            pll0pll1[chain_num] = (pll0pll1[chain_num]+1)%2;            
            break;
        default:
            break;
    }

    return ret;
}

/*for test, not used in mining.*/
/*
int bypass_asic_freq_by_chain(GlobalState *state, int chain_num)
{
    int ret = 0;
    uint8_t pll_reg_addr = 0;

    if(0 == pll0pll1){
        pll_reg_addr = REG_PLL1_CONFIG;
    }else if(1 == pll0pll1){
        pll_reg_addr = REG_PLL0_CONFIG;
    }else{
        ESP_LOGW(TAG, "pll_index %u Error.", pll0pll1);
        ret = -1;
        goto exit;
    }

    if(state->chain_pluged[chain_num]){
        pll_config_data param = calc_pll_param(100, state->real_freq + chain_num);
        ESP_LOGI(TAG, "chain %d, bypass", chain_num);

        //set bypass mode:
        param.pll_resetn = 1;
        param.pll_bypass = 1;
        set_top_reg(chain_num, true, 0, pll_reg_addr, *(uint32_t *)(&param), true);
        volc_delay(5);
    }

exit:
    return ret;
}
*/

void softreset_asic_by_chain(GlobalState *state, int chain_num)
{
    if(state->chain_pluged[chain_num]){
        set_top_reg(chain_num, true, 0, REG_SOFT_RESET, 1<<2, false);
        volc_delay(100);
        set_top_reg(chain_num, true, 0, REG_SOFT_RESET, 0, false);
    } 
}

void softreset_all_asic(GlobalState *state)
{
    for(int chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++){
        softreset_asic_by_chain(state, chain_num);
    }
}

void config_tmmusk_by_chain(GlobalState *state, int chain_num, uint32_t tmmusk)
{
    if(state->chain_pluged[chain_num]){
        set_core_reg(chain_num, true, 0, true, 0, COREREG_TICKMASK, tmmusk, true);
    }    
}

void config_all_tmmusk(GlobalState *state, uint32_t tmmusk)
{
    for(int chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++){
        config_tmmusk_by_chain(state, chain_num, tmmusk);
    }    
}

#if 1
void config_sram_by_chain(GlobalState *state, int chain_num)
{
    uint32_t sram_param = 0x00140000; //0x111600;

    if(state->chain_pluged[chain_num]){
        set_core_reg(chain_num, true, 0, true, 0, COREREG_SRAMCTRL0, sram_param, true);
        set_core_reg(chain_num, true, 0, true, 0, COREREG_SRAMCTRL1, sram_param, true);
    }

    ESP_LOGI(TAG, "Configure SRAM %06x.", sram_param);
}
#else
void config_sram_by_chain(GlobalState *state, int chain_num)
{
    uint32_t sram_param = 0;
    int opt_sram_vol = 1;

    if(state->chain_pluged[chain_num]){
        if(state->asic_freqency >= 1900){
            sram_param = 0x140000;
        }else if(state->asic_freqency >= 1650){
            //sram_param = 0x131400;
            sram_param = 0x111600;
        }else{
            sram_param = 0x101600|opt_sram_vol<<16;
        }

        set_core_reg(chain_num, true, 0, true, 0, COREREG_SRAMCTRL0, sram_param, true);
        set_core_reg(chain_num, true, 0, true, 0, COREREG_SRAMCTRL1, sram_param, true);
    }

    ESP_LOGI(TAG, "Configure SRAM %06x.", sram_param);
}
#endif

void config_all_sram(GlobalState *state)
{   
    for(int chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++){
        config_sram_by_chain(state, chain_num);
    }    
}

/*
7010: 25M:0 12.5M:1 3.125M:7 115200:216(0xD8)
50M clock: 115200:53 3.125M:1 12.5M:3 25M:1
25M clock: 115200:26 3.125M:0 12.5M:3 25M:1
*/

int getBTD(int baud){
	if(baud==25000000) {
		return 1;
	}else if(baud==12500000){
		return 3;
	}else if(baud==3125000){
		#if defined(BASE_25M)
			return 0;
		#elif defined(BASE_50M)
			return 0;
		#endif
	}else if(baud==115200){
		#if defined(BASE_25M)
			return 26;
		#elif defined(BASE_50M)
			return 26;
		#endif
	}else{
		ESP_LOGW(TAG,"not support baud\n");
	}

	return -1;
}

void config_max_baud_by_chain(int chain_num)
{
    /*set baudrate 3125000.*/
    set_top_reg(chain_num, true, 0, REG_UART_BAND, 0<<16|getBTD(3125000), false);
}

void config_baud_rate_by_chain(int chain_num, uint32_t baud_rate)
{
    set_top_reg(chain_num, true, 0, REG_UART_BAND, 0<<16|getBTD(baud_rate), false);
}

void config_uartgap(int chain_num, uint32_t regValue)
{
    set_top_reg(chain_num, true, 0, REG_UART_GAP, regValue, false);
    volc_delay(5);
}


int LT0051_set_max_baud_by_chain(GlobalState *state, int chain_num)
{
    int ret = 0;
    int uart_num = chain_num + 1;
    uint32_t baud_rate = 3125000;

    switch (state->baud_rate[chain_num])
    {
    case 115200:
        baud_rate = 115200;
        return baud_rate;
        break;
    case 230400:
        baud_rate = 230400;
        /* code */
        break;
    case 460800:
        baud_rate = 460800;
        /* code */
        break;
    case 3125000:
        baud_rate = 460800;
        /* code */
        break;
    default:
        baud_rate = 3125000;
        break;
    }

    /*set the baudrate of hashoard.*/
    config_baud_rate_by_chain(chain_num, baud_rate);

    volc_delay(10);
    /*change the uart configuration of esp32.*/
    // 1. 配置UART参数
    uart_config_t uart_config = {
        .baud_rate = baud_rate,  // 设置新的波特率
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    // 2. 应用UART配置
    esp_err_t err = uart_param_config(uart_num, &uart_config);
    if (err != ESP_OK) {
        // 处理配置失败的情况
        ESP_LOGW(TAG, "Failed to configure UART parameters (Error code: %d)\n", err);
        ret = -1;
        return ret;
    }   
    // 3. 可选：刷新UART缓冲区
    uart_flush(uart_num);
    
    ESP_LOGI(TAG, "UART%d baudrate changed to %d", uart_num, baud_rate);

    return baud_rate;
}

void config_all_max_baud(GlobalState *state)
{
    for(int chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++){
        if(state->chain_pluged[chain_num]){
            LT0051_set_max_baud_by_chain(state, chain_num);
        }
    }
}

esp_err_t LT0051_init(GlobalState *state, uint64_t frequency, uint16_t asic_count)
{
    ESP_LOGI(TAG, "LT0051_init Initializing");
    int ret = ESP_OK;
    bool bCheck = false;

    /*check pll status and check the chip number.*/
    bCheck = check_all_pll_status(state, asic_count);
    if(!bCheck){
        ret = ESP_FAIL;
        goto exit;
    }else{
        ESP_LOGI(TAG, "Pass the pll-check-1.");
    }

    /*change the uart baudrate.*/
    config_all_max_baud(state);

    /*change the asic address.*/
    ESP_LOGI(TAG, "Configure asic address.");
    config_all_asic_address(state, asic_count);
    
    /*set pll/freq */
    ESP_LOGI(TAG, "Configure frequency.");
    if(pll0pll1[0] != pll0pll1[1]){
        ESP_LOGW(TAG, "pll0pll1[0] != pll0pll1[1]");
    }
    config_all_asic_freq(state, frequency, pll0pll1[0]);
    for(int i = 0; i < MAX_CHAIN_NUM; i++){
        pll0pll1[i] = (pll0pll1[i]+1)%2;
    }

    /*softreset */
    ESP_LOGI(TAG, "Softreset all asics.");
    softreset_all_asic(state);
    volc_delay(1000);

    /*check pll status and check the chip number.*/
    bCheck = check_all_pll_status(state, asic_count);
    if(!bCheck){
        ret = ESP_FAIL;
        goto exit;
    }else{
        ESP_LOGI(TAG, "Pass the pll-check-2.");
    }

    /*config tmmusk*/
    ESP_LOGI(TAG, "Configure the tmmusk %"PRIX32".", state->tmmusk);
    config_all_tmmusk(state, state->tmmusk);

    /*config sram0 and sram1 */
    ESP_LOGI(TAG, "Configure the sram");
    config_all_sram(state);
exit:
    return ret;
}

esp_err_t LT0051_init_by_chain(GlobalState *state, int chain_num, uint64_t frequency, uint16_t asic_count)
{
    ESP_LOGI(TAG, "LT0051_init Initializing");
    int ret = ESP_OK;
    bool bCheck = false;

    /*check pll status and check the chip number.*/
    bCheck = check_pll_status_by_chain(state, chain_num, asic_count);
    if(!bCheck){
        ret = ESP_FAIL;
        goto exit;
    }else{
        ESP_LOGI(TAG, "Pass the pll-check-1.");
    }

    ESP_LOGI(TAG, "LT0051_init baud_rate=%d  expected asic_count=%d", state->baud_rate[chain_num], asic_count);

    /*config the uart gap.*/
    config_uartgap(chain_num, 0x00FF000F);

    /*change the uart baudrate.*/
    LT0051_set_max_baud_by_chain(state, chain_num);

    /*change the asic address.*/
    ESP_LOGI(TAG, "Chain %d: Configure asic address", chain_num);
    config_asic_address_by_chain(state, chain_num, asic_count);

    /*set pll/freq */
    ESP_LOGI(TAG, "Configure frequency.");
    config_asic_freq_by_chain(state, chain_num, frequency, pll0pll1[chain_num]);
    pll0pll1[chain_num] = (pll0pll1[chain_num] + 1) % 2;

    /*softreset */
    ESP_LOGI(TAG, "Softreset asics.");
    softreset_asic_by_chain(state, chain_num);
    volc_delay(1000);

    /*check pll status and check the chip number.*/
    bCheck = check_pll_status_by_chain(state, chain_num, asic_count);
    if(!bCheck){
        ret = ESP_FAIL;
        goto exit;
    }else{
        ESP_LOGI(TAG, "Pass the pll-check-2.");
    }

    /*config tmmusk*/
    ESP_LOGI(TAG, "Configure the tmmusk %"PRIX32".", state->tmmusk);
    config_tmmusk_by_chain(state, chain_num, state->tmmusk);

    /*config sram0 and sram1 */
    ESP_LOGI(TAG, "Configure the sram");
    config_sram_by_chain(state, chain_num);

exit:
    return ret;
}

int LT0051_dummy_init(GlobalState *state, uint64_t frequency, uint16_t asic_count)
{
    ESP_LOGI(TAG, "Initializing");

    for(uint32_t chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++){
        state->chain_pluged[chain_num] = true;
        state->asic_count[chain_num] = asic_count;
    }

    return ESP_OK;
}

int LT0051_set_default_baud(GlobalState *state)
{
    //TODO:
    return 115200;
}

bool LT0051_send_hash_frequency(GlobalState *state, float frequency)
{
    int ret = 0;
    uint16_t u_freq = (uint16_t)frequency;

    if(pll0pll1[0] != pll0pll1[1]){
        ESP_LOGI(TAG, "pll0pll1[0] != pll0pll1[1]");
        return false;
    }

    ret = config_all_asic_freq(state, u_freq, pll0pll1[0]);
    pll0pll1[0] = (pll0pll1[0]+1)%2;
    pll0pll1[1] = (pll0pll1[1]+1)%2;
    ESP_LOGI(TAG, "LT0051 set frequency %f", frequency);

    if(0 != ret){
        return false;
    }else{
        return true;
    } 
}

void LT0051_set_job_difficulty_mask(GlobalState *state, int difficulty)
{
    //TODO: config tm when init.
}

/*
New job: 
pre_block in Block.one: 
9bb865023950aae5a90c57b4338699639b5ca010c9b31b285fc13e425b39d15d
pre_block in stratum:
5b39d15d5fc13e42c9b31b289b5ca01033869963a90c57b43950aae59bb86502

total_data:
a386011ab2bb6d603a7a267595bbe622c13ad7b66c7d7fad2188960ba005998fd89d97f19d0dcd990265b89be5aa5039b4570ca96399863310a05c9b281bb3c9423ec15f5dd1395b00000020
a386011a b2bb6d60 3a7a267595bbe622c13ad7b66c7d7fad2188960ba005998fd89d97f19d0dcd99 0265b89be5aa5039b4570ca96399863310a05c9b281bb3c9423ec15f5dd1395b 00000020
work_id is 2
before scramble and crc: 
abcd01005801000000000200000000a386011ab2bb6d603a7a267595bbe622c13ad7b66c7d7fad2188960ba005998fd89d97f19d0dcd990265b89be5aa5039b4570ca96399863310a05c9b281bb3c9423ec15f5dd1395b00000020
**********************************
in Block.one： 0b2b9fd9ae5d5916cc60382e9870d693d8347045dbdfbd552e50126ad013d136
stratum: d013d1362e50126adbdfbd55d83470459870d693cc60382eae5d59160b2b9fd9
*/
void LT0051_send_work(GlobalState * GLOBAL_STATE, bm_job * next_bm_job, uint32_t chain_num, uint8_t workid)
{
    uint8_t cmd_buf[MS_HASHJOB1_LEN] = {0};
    uint8_t job1_data[MS_HASHJOB_DATA_LEN] = {0};

    /*construct the hashjob1 data from bm_job.*/
    //memcpy(job1_data, &(next_bm_job->version), 4);
    enc32be(job1_data, next_bm_job->version);
    memcpy(job1_data + 4, next_bm_job->prev_block_hash_be, 32);
    memcpy(job1_data + 4 + 32, next_bm_job->merkle_root_be, 32);
    enc32be(job1_data + 4 + 32 + 32, next_bm_job->ntime);
    enc32be(job1_data + 4 + 32 + 32 + 4, next_bm_job->target);
    reverse_bytes(job1_data, MS_HASHJOB_DATA_LEN);

    #ifdef LT0051_DEBUG
    //prettyHex(next_bm_job->prev_block_hash_be, 32);
    ESP_LOGI(TAG, "chain_num %"PRIu32", work id %"PRIu8"", chain_num, workid);
    prettyHex(job1_data, MS_HASHJOB_DATA_LEN);
    #endif
    /*construct the hashjob1*/
    pack_ms_job_hashJob1(cmd_buf, 1, 0, 1, 0, workid, job1_data, 0x00000000);

    /*
     * Free and replace the slot under valid_jobs_lock, not beside it. The
     * result task reads active_jobs[] under this lock and copies out what a
     * share needs; doing the free outside it let a nonce that had just passed
     * the valid_jobs check be scored and submitted from memory this call had
     * already handed back to the heap. Freeing the displaced job after the
     * unlock is safe, because nothing can still reach it: the slot no longer
     * points at it, and any reader that held the lock has finished copying.
     */
    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

    bm_job * displaced = GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[workid];
    GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[workid] = next_bm_job;
    GLOBAL_STATE->valid_jobs[chain_num][workid] = 1;

    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

    if (displaced != NULL) {
        free_bm_job(displaced);
    }

    SERIAL_send(chain_num, cmd_buf, MS_HASHJOB1_LEN, true);
}

void LT0051_send_work_by_chip(
    GlobalState * GLOBAL_STATE, bm_job *next_bm_job, 
    uint32_t chain_num, uint8_t workid, uint16_t asic_count)
{
    uint8_t cmd_buf[MS_HASHJOB1_LEN] = {0};
    uint8_t job1_data[MS_HASHJOB_DATA_LEN] = {0};
    uint8_t addr_interval = 0x100 / asic_count;

    /*construct the hashjob1 data from bm_job.*/
    //memcpy(job1_data, &(next_bm_job->version), 4);
    enc32be(job1_data, next_bm_job->version);
    memcpy(job1_data + 4, next_bm_job->prev_block_hash_be, 32);
    memcpy(job1_data + 4 + 32, next_bm_job->merkle_root_be, 32);
    enc32be(job1_data + 4 + 32 + 32, next_bm_job->ntime);
    enc32be(job1_data + 4 + 32 + 32 + 4, next_bm_job->target);
    reverse_bytes(job1_data, MS_HASHJOB_DATA_LEN);

    #ifdef LT0051_DEBUG
    //prettyHex(next_bm_job->prev_block_hash_be, 32);
    ESP_LOGI(TAG, "chain_num %"PRIu32", work id %"PRIu8"", chain_num, workid);
    prettyHex(job1_data, MS_HASHJOB_DATA_LEN);
    #endif

    /*
     * Free and replace the slot under valid_jobs_lock, not beside it. The
     * result task reads active_jobs[] under this lock and copies out what a
     * share needs; doing the free outside it let a nonce that had just passed
     * the valid_jobs check be scored and submitted from memory this call had
     * already handed back to the heap. Freeing the displaced job after the
     * unlock is safe, because nothing can still reach it: the slot no longer
     * points at it, and any reader that held the lock has finished copying.
     */
    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

    bm_job * displaced = GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[workid];
    GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[workid] = next_bm_job;
    GLOBAL_STATE->valid_jobs[chain_num][workid] = 1;

    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

    if (displaced != NULL) {
        free_bm_job(displaced);
    }

    for(uint16_t asic_index = 0; asic_index < asic_count; asic_index++){
        uint16_t asic_addr = asic_index * addr_interval;
        /*construct the hashjob1*/
        pack_ms_job_hashJob1(cmd_buf, 0, asic_addr, 1, 0, workid, job1_data, 0x00000000);

        SERIAL_send(chain_num, cmd_buf, MS_HASHJOB1_LEN, true);
    }
}

void LT0051_send_work_by_cores_group(
    GlobalState * GLOBAL_STATE, bm_job *next_bm_job, 
    uint32_t chain_num, uint8_t workid, 
    uint8_t core_groups, uint8_t core_id_mask)
{
    uint8_t cmd_buf[MS_HASHJOB1_LEN] = {0};
    uint8_t job1_data[MS_HASHJOB_DATA_LEN] = {0};

    /*construct the hashjob1 data from bm_job.*/
    //memcpy(job1_data, &(next_bm_job->version), 4);
    enc32be(job1_data, next_bm_job->version);
    memcpy(job1_data + 4, next_bm_job->prev_block_hash_be, 32);
    memcpy(job1_data + 4 + 32, next_bm_job->merkle_root_be, 32);
    enc32be(job1_data + 4 + 32 + 32, next_bm_job->ntime);
    enc32be(job1_data + 4 + 32 + 32 + 4, next_bm_job->target);
    reverse_bytes(job1_data, MS_HASHJOB_DATA_LEN);

    #ifdef LT0051_DEBUG
    //prettyHex(next_bm_job->prev_block_hash_be, 32);
    ESP_LOGI(TAG, "chain_num %"PRIu32", work id %"PRIu8"", chain_num, workid);
    prettyHex(job1_data, MS_HASHJOB_DATA_LEN);
    #endif

    /*
     * Free and replace the slot under valid_jobs_lock, not beside it. The
     * result task reads active_jobs[] under this lock and copies out what a
     * share needs; doing the free outside it let a nonce that had just passed
     * the valid_jobs check be scored and submitted from memory this call had
     * already handed back to the heap. Freeing the displaced job after the
     * unlock is safe, because nothing can still reach it: the slot no longer
     * points at it, and any reader that held the lock has finished copying.
     */
    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

    bm_job * displaced = GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[workid];
    GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[workid] = next_bm_job;
    GLOBAL_STATE->valid_jobs[chain_num][workid] = 1;

    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

    if (displaced != NULL) {
        free_bm_job(displaced);
    }

    for(uint8_t core_index = 0; core_index < core_groups; core_index++){
        /*construct the hashjob1*/
        pack_ms_core_job_hashJob1(
            cmd_buf, 1, 0, 0, core_index, core_id_mask, workid, job1_data, 0x00000000
        );

        SERIAL_send(chain_num, cmd_buf, MS_HASHJOB1_LEN, true);
    }
}

static task_result result[MAX_CHAIN_NUM];
task_result* LT0051_process_work(GlobalState * GLOBAL_STATE, uint32_t chain_num)
{
    struct nonce_response asic_response = {0};

    if (ESP_FAIL == receive_work((uint8_t *)&asic_response, sizeof(asic_response), chain_num)){
        return NULL;
    }

    uint8_t rx_job_id = asic_response.wc & 0x7f;
    if(0 == GLOBAL_STATE->valid_jobs[chain_num][rx_job_id]){
#ifdef LT0051_DEBUG
        ESP_LOGW(TAG, "Invalid job nonce found, id=%d, chain=%"PRIu32"", rx_job_id, chain_num);
        ESP_LOGW(TAG, "%"PRIu32" %"PRIu8" %"PRIu8" %"PRIu8".", 
                    asic_response.nonce, asic_response.wc, asic_response.chip_id, asic_response.core_id);
        char dump_buffer[50];
        bin2hex((const uint8_t *)(&asic_response), sizeof(struct nonce_response), dump_buffer, 2*sizeof(struct nonce_response)+1);
        ESP_LOGW(TAG, "response: %s", dump_buffer);
#endif
        return NULL;
    }

    result[chain_num].job_id = rx_job_id;
    result[chain_num].nonce = asic_response.nonce;
    result[chain_num].chip_id = asic_response.chip_id;
    result[chain_num].core_id = asic_response.core_id;

    return &(result[chain_num]);
}
