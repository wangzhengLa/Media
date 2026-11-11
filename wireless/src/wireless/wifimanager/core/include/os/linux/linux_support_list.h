#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wifi_log.h>
#include <wmg_common.h>
#include <linux_get_config.h>

#define WPA_HOSTAPD_COEXIST 0b100
#define TWO_WPA_COEXIST     0b001

uint8_t get_linux_current_module_indep_bitmap(void);

/*
 * Returns the matching corresponding number
 * If it is -1, it means no match was found
 * */
int find_match_support_list(char* module_name);

uint8_t* module_mode_support_list_register(void);
