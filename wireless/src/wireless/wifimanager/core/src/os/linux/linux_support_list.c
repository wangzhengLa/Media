#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <wifi_log.h>
#include <wmg_common.h>
#include <linux_get_config.h>
#include <linux_support_list.h>

#define STA_ALONE                 1
#define AP_ALONE                  2
#define STA_AP_COEXIST            3
#define MON_ALONE                 4
#define STA_MON_COEXIST           5
#define AP_MON_COEXIST            6
#define STA_AP_MON_COEXIST        7
#define P2P_ALONE                 8
#define STA_P2P_COEXIST           9
#define AP_P2P_COEXIST            10
#define STA_AP_P2P_COEXIST        11
#define MON_P2P_COEXIST           12
#define STA_MON_P2P_COEXIST       13
#define AP_MON_P2P_COEXIST        14
#define STA_AP_MON_P2P_COEXIST    15

#define LINUX_MODE_SUPPORT_LIST(fmt, ...) {fmt,__VA_ARGS__}

typedef struct {
	char module_name[MODULE_NAME_MAX_LEN];
	uint8_t module_mode_support_list[MAX_SUPPORT_MODE_NUM];
	uint8_t wpad_indep_bitmap;
} linux_module_t;

/*
 * 0b010 -> AIC8800 (default; similar for other modules)
 * bit0:(0)only use one wpa_supplicant
 * bit1:(1)sta and p2p can coexist(if only sta or p2p is closed, wpa_supplicant cannot be close)
 * bit2:(0)wpa_supplicant and hostapd cannot exist at the same time
 */
static linux_module_t linux_module_def = {
	.module_name = "DEFMODULE",
	.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE),
	.wpad_indep_bitmap = 0,
};

/*
 * Module name definition requirements
 * 1.Module real name
 * 2.Kernel version
 * 3.Special characters(Try to define as relevant to the solution as possible)
 * 4.Content is divided using "-"
 * For example: AIC8800-5.10-NORMAL
 * AIC8800: The real name of the module
 * 5.10: The kernel version of the solution module
 * NORMAL: support STA+AP normal mode
 */
static linux_module_t linux_module_mode_support_list[] = {
#ifdef WIRELESS_MODULE_AIC8800
#ifdef LINUX_VERSION_4_9
	{
		.module_name = "AIC8800-4.9-NORMAL",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE),
		.wpad_indep_bitmap = 0,
	},
#endif

#ifdef LINUX_VERSION_5_4
	{
		.module_name = "AIC8800-5.4-NORMAL",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE),
		.wpad_indep_bitmap = 0,
	},
#endif

#ifdef LINUX_VERSION_5_10
	{
		.module_name = "AIC8800-5.10-NORMAL",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE),
		.wpad_indep_bitmap = 0,
	},
#endif

#ifdef LINUX_VERSION_5_15
	{
		.module_name = "AIC8800-5.15-NORMAL",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE),
		.wpad_indep_bitmap = 0,
	},

	{
		.module_name = "AIC8800-5.15-SA",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE, STA_AP_COEXIST),
		.wpad_indep_bitmap = WPA_HOSTAPD_COEXIST,
	},
#endif

#ifdef LINUX_VERSION_6_6
	{
		.module_name = "AIC8800-6.6-NORMAL",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE),
		.wpad_indep_bitmap = 0,
	},

	{
		.module_name = "AIC8800-6.6-SA",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE, STA_AP_COEXIST),
		.wpad_indep_bitmap = WPA_HOSTAPD_COEXIST,
	},

	{
		.module_name = "AIC8800-6.6-P2P",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE, P2P_ALONE),
		.wpad_indep_bitmap = WPA_HOSTAPD_COEXIST,
	},
#endif
#endif

	/* Default configuration, please do not modify it casually */
	{
		.module_name = "DEFMODULE",
		.module_mode_support_list = LINUX_MODE_SUPPORT_LIST(STA_ALONE, AP_ALONE),
		.wpad_indep_bitmap = 0,
	}
};

static const linux_module_t *linux_current_module_p = NULL;

static void register_linux_current_module(linux_module_t *module_list_node_p)
{
	linux_current_module_p = module_list_node_p;
}

uint8_t get_linux_current_module_indep_bitmap(void)
{
	return linux_current_module_p->wpad_indep_bitmap;
}

static int get_linux_module_info(char *module_name, linux_module_t **module_list_node_p)
{
	int node_num = 0, i = 0;
	wmg_bool_t match_flag = WMG_FALSE;
	uint8_t tmp_module_mode_support_list[MAX_SUPPORT_MODE_NUM] = {0};

	*module_list_node_p = &linux_module_def;
	while(strcmp("DEFMODULE", linux_module_mode_support_list[node_num].module_name) && (match_flag == WMG_FALSE)) {
		/* Match to the corresponding support list */
		if(!strcmp(linux_module_mode_support_list[node_num].module_name, module_name)) {
			*module_list_node_p = &linux_module_mode_support_list[node_num];
			match_flag = WMG_TRUE;
		}
		node_num++;
	}

	/* Update the sorting of the match list */
	memcpy(tmp_module_mode_support_list, (*module_list_node_p)->module_mode_support_list, sizeof(uint8_t) * 16);
	memset((*module_list_node_p)->module_mode_support_list, 0, sizeof(uint8_t) * 16);
	for(i = 0; i < MAX_SUPPORT_MODE_NUM; i++){
		if(tmp_module_mode_support_list[i] != 0) {
			(*module_list_node_p)->module_mode_support_list[(tmp_module_mode_support_list[i])] = tmp_module_mode_support_list[i];
		}
	}

	if(match_flag) {
		return node_num;
	} else {
		return -1;
	}
}

/*
 * Returns the matching corresponding number
 * If it is -1, it means no match was found
 * */
int find_match_support_list(char* module_name)
{
	linux_module_t* module_list_node_p = NULL;
	return get_linux_module_info(module_name, &module_list_node_p);
}

uint8_t* module_mode_support_list_register(void)
{
	char module_name[MODULE_NAME_MAX_LEN];
	linux_module_t *module_list_node_p = NULL;

	get_config("wifi_module_name", module_name, "UNKNOWN");
	get_linux_module_info(module_name, &module_list_node_p);
	register_linux_current_module(module_list_node_p);
	return module_list_node_p->module_mode_support_list;
}
