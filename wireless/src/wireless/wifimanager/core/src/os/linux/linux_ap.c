#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <poll.h>
#include <pthread.h>
#include <wpa_ctrl.h>
#include <udhcpc.h>
#include <utils.h>
#include <wifi_log.h>
#include <wifimg.h>
#include <wmg_ap.h>
#include <unistd.h>
#include <linux_ap.h>
#include <linux_common.h>
#include "scan.h"

static wmg_ap_inf_object_t ap_inf_object;

#define AP_SEC_NONE        0
#ifndef CONFIG_PATH_MAX
#define CONFIG_PATH_MAX 4096
#endif

#define HOSTAPD_TIMEOUT      3000000  // microseconds
#define HOSTAPD_TIMEOUT_STEP  100000  // microseconds

#define CHANNEL_5G_NUM     14
int allowed_5g_channel[CHANNEL_5G_NUM] = {36, 44, 52, 60, 100, 108, 116, 124, 132, 140, 149, 157, 184, 192};

typedef struct {
	wifi_ap_config_t *ap_config;
	bool ap_config_enable;
} global_ap_config_t;

static char global_ssid[SSID_MAX_LEN + 1] = {0};
static char global_psk[PSK_MAX_LEN + 1] = {0};

static wifi_ap_config_t global_wifi_ap_config = {
	.ssid = global_ssid,
	.psk = global_psk,
	.sec = WIFI_SEC_NONE,
	.channel = 0,
	.dev_list = { NULL },
	.sta_num = 0,
};

static global_ap_config_t global_ap_config = {
	.ap_config = &global_wifi_ap_config,
	.ap_config_enable = false,
};

static wmg_status_t parse_ap_config(char *reply, wifi_ap_config_t *ap_config)
{
	char *pos = NULL;
	char utf8_tmp_buf[SSID_MAX_LEN * 4] = {0};
	char *pch;

	if (reply == NULL || ap_config == NULL) {
		WMG_ERROR("invalid parameters\n");
		return WMG_STATUS_INVALID;
	}

	pos = strstr(reply, "state");
	if (pos != NULL) {
		pos += 6;
		if (strncmp(pos, "ENABLED", 7) != 0) {
			WMG_WARNG("Warning: ap state is inactive\n");
			return WMG_STATUS_FAIL;
		} else {
			WMG_DUMP("ap state is enable\n");
			pch = strtok(reply, "'\n'");
			while (pch != NULL) {
				if (strncmp(pch, "ssid[0]=", 8) == 0) {
					if((strlen(pch + 8) <= (SSID_MAX_LEN * 4)) && (convert_utf8((pch + 8), utf8_tmp_buf) <= SSID_MAX_LEN)) {
						strcpy(ap_config->ssid, utf8_tmp_buf);
						WMG_DEBUG("%s\n", ap_config->ssid);
					} else {
						WMG_DEBUG("ssid is too long:%d",strlen(pch + 8));
						return WMG_STATUS_INVALID;
					}
				}
				if (strncmp(pch, "channel=", 8) == 0) {
					ap_config->channel = atoi(pch + 8);
					WMG_DEBUG("%d\n", ap_config->channel);
				}
				if (strncmp(pch, "num_sta[0]=", 11) == 0) {
					ap_config->sta_num = atoi(pch + 11);
					WMG_DEBUG("%d\n", ap_config->sta_num);
				}
				pch = strtok(NULL, "'\n'");
			}
			return WMG_STATUS_SUCCESS;
		}
	} else {
		WMG_ERROR("invalid ap config info %s\n", reply);
	}

	return WMG_STATUS_FAIL;
}

static wmg_status_t linux_command_to_hostapd(char const *cmd, char *reply, size_t reply_len)
{
	return command_to_wpad(cmd, reply, reply_len, WIFI_AP, NULL);
}

static wmg_status_t linux_ap_set_config(wifi_ap_config_t *ap_config)
{
	char cmd[CMD_MAX_LEN * 2] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	int passwd_len = 0;
	uint8_t ip_addr[4] = {0};
	int ap_sec_wap = 0;
	int channel_5g_num = 0, legal_flag = 0;
	char wpa_or_rsn_pairwise[32] = {0};

	if((ap_config == NULL) || (ap_config->ssid == NULL)){
		WMG_ERROR("ap config format is error, ssid can't NULL\n");
		return WMG_STATUS_FAIL;
	}

	WMG_DUMP("set ssid %s\n", ap_config->ssid);
	sprintf(cmd, "SET ssid %s", ap_config->ssid);
	if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to set ssid '%s', reply %s\n",ap_config->ssid, reply);
		return WMG_STATUS_FAIL;
	}

	if (ap_config->channel < 14) {
		WMG_DUMP("set channel %u\n", ap_config->channel);
		sprintf(cmd, "SET channel %u", ap_config->channel);
		if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
			WMG_ERROR("failed to set channel '%u', reply %s\n", ap_config->channel, reply);
			return WMG_STATUS_FAIL;
		}
	} else {
		for (channel_5g_num = 0; channel_5g_num < CHANNEL_5G_NUM; channel_5g_num++) {
			if (ap_config->channel == allowed_5g_channel[channel_5g_num]) {
				legal_flag = 1;
				break;
			}
		}
		if (legal_flag) {
			WMG_DUMP("set hw_mode a\n");
			sprintf(cmd, "SET hw_mode a");
			if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set hw_mode a, reply %s\n", reply);
				return WMG_STATUS_FAIL;
			}

			WMG_DUMP("set channel %u\n", ap_config->channel);
			sprintf(cmd, "SET channel %u", ap_config->channel);
			if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set channel '%u', reply %s\n", ap_config->channel, reply);
				return WMG_STATUS_FAIL;
			}

			WMG_DUMP("set ieee80211n\n");
			sprintf(cmd, "SET ieee80211n 1");
			if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set ieee80211n 1, reply %s\n", reply);
				return WMG_STATUS_FAIL;
			}
		} else {
			WMG_ERROR("invalid channel(%d)\n", ap_config->channel);
			return WMG_STATUS_FAIL;
		}
	}

	switch (ap_config->sec){
		case WIFI_SEC_NONE:
			WMG_DUMP("psk is NULL, set wpa NONE\n");
			sprintf(cmd, "SET wpa %d", AP_SEC_NONE);
			if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set wpa AP_SEC_NONE, reply %s\n", reply);
				return WMG_STATUS_FAIL;
			}
			break;
		case WIFI_SEC_WEP:
			sprintf(cmd, "SET wep_default_key 0");
			if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set wpa_default_key 0, reply %s\n");
				return WMG_STATUS_FAIL;
			}
			WMG_DUMP("set wep_default_key 0\n");
			passwd_len = strlen(ap_config->psk);
			if((passwd_len == 10) || (passwd_len == 26)) {
				sprintf(cmd, "SET wep_key0 %s", ap_config->psk);
				WMG_DEBUG("set passwd HEX format!\n");
			} else if((passwd_len == 5) || (passwd_len == 13)) {
				sprintf(cmd, "SET wep_key0 \"%s\"", ap_config->psk);
				WMG_DEBUG("The passwd is ASCII format!\n");
			} else {
				WMG_ERROR("The password does not conform to the specification!\n");
				return WMG_STATUS_FAIL;
			}
			if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set wep_key0 '%s', reply %s\n", ap_config->psk, reply);
				return WMG_STATUS_FAIL;
			}
			WMG_DUMP("set wep_key0 %s\n", ap_config->psk);
			break;
		case (WIFI_SEC_WPA_PSK | WIFI_SEC_WPA2_PSK):
		case WIFI_SEC_WPA_PSK:
		case WIFI_SEC_WPA2_PSK:
			if (ap_config->psk != NULL) {
				/* check whether the length is legal */
				passwd_len = strlen(ap_config->psk);
				if(passwd_len < 8) {
					WMG_ERROR("Password shorter than 8 characters!\n");
					return WMG_STATUS_FAIL;
				} else if (passwd_len < 64) {
					sprintf(cmd, "SET wpa_passphrase %s", ap_config->psk);
				} else {
					WMG_ERROR("Password longer than 63 characters!\n");
					return WMG_STATUS_FAIL;
				}

				if (linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
					WMG_ERROR("failed to set psk '%s', reply %s\n",ap_config->psk, reply);
					return WMG_STATUS_FAIL;
				}
				WMG_DUMP("set psk %s\n", ap_config->psk);

				sprintf(cmd, "SET wpa_key_mgmt WPA-PSK");
				if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
					WMG_ERROR("failed to set wpa key mgmt WPA-PSK, reply %s\n", reply);
					return WMG_STATUS_FAIL;
				}

				if(ap_config->sec == WIFI_SEC_WPA_PSK){
					ap_sec_wap = 1;
					strcpy(wpa_or_rsn_pairwise, "wpa_pairwise TKIP");
				} else if(ap_config->sec == WIFI_SEC_WPA2_PSK){
					ap_sec_wap = 2;
					strcpy(wpa_or_rsn_pairwise, "rsn_pairwise CCMP");
				} else {
					ap_sec_wap = 3;
					strcpy(wpa_or_rsn_pairwise, "wpa_pairwise TKIP CCMP");
				}

				/* only WPA wpa=1, only WPA2 wpa=2, WPA/WPA2 wpa=3 */
				sprintf(cmd, "SET wpa %d", ap_sec_wap);
				if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
					WMG_ERROR("failed to set wpa %d, reply %s\n", ap_sec_wap, reply);
					return WMG_STATUS_FAIL;
				} else {
					WMG_DUMP("set wpa %d\n", ap_sec_wap);
				}

				sprintf(cmd, "SET %s", wpa_or_rsn_pairwise);
				if(linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
					WMG_ERROR("failed to set %s", wpa_or_rsn_pairwise);
					return WMG_STATUS_FAIL;
				} else {
					WMG_DUMP("set %s\n", wpa_or_rsn_pairwise);
				}
			} else {
				WMG_ERROR("psk is NULL, but sec not WIFI_SEC_NONE(WIFI_SEC_WPA_PSK)\n");
				WMG_ERROR("Please check ap config format\n");
				return WMG_STATUS_FAIL;
			}
			break;
		default:
			WMG_ERROR("not support sec\n");
			WMG_ERROR("Please check ap config format\n");
			return WMG_STATUS_FAIL;
			break;
	}

	if(memcmp(ap_config->ip_addr, ip_addr, 4) != 0){
		memcpy(global_ap_config.ap_config->ip_addr, ap_config->ip_addr, 4);
	} else {
		global_ap_config.ap_config->ip_addr[0] = 192;
		global_ap_config.ap_config->ip_addr[1] = 168;
		global_ap_config.ap_config->ip_addr[2] = 5;
		global_ap_config.ap_config->ip_addr[3] = 1;
	}

	memcpy(global_ap_config.ap_config->gw_addr, ap_config->ip_addr, 4);
	/* WIFI_SEC_NONE and public callers may supply NULL ssid/psk */
	strcpy(global_ap_config.ap_config->ssid, (ap_config->ssid != NULL) ? ap_config->ssid : "");
	strcpy(global_ap_config.ap_config->psk, (ap_config->psk != NULL) ? ap_config->psk : "");
	global_ap_config.ap_config->sec = ap_config->sec;
	global_ap_config.ap_config->channel = ap_config->channel;
	global_ap_config.ap_config_enable = true;

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_ap_cmd_enable()
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};

	sprintf(cmd, "%s", "ENABLE");
	cmd[CMD_MAX_LEN - 1] = '\0';
	if (linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to enable ap, reply %s\n", reply);
		return WMG_STATUS_FAIL;
	}
	return WMG_STATUS_SUCCESS;
}

static int linux_ap_cmd_disable()
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};

	sprintf(cmd, "%s", "DISABLE");
	cmd[CMD_MAX_LEN - 1] = '\0';
	if (linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to disable ap, reply %s\n", reply);
		return WMG_STATUS_FAIL;
	}
	return WMG_STATUS_SUCCESS;
}

static int linux_ap_cmd_status()
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};

	sprintf(cmd, "%s", "STATUS");
	cmd[CMD_MAX_LEN - 1] = '\0';
	if (linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to get ap status\n");
		return -1;
	}
	if (strstr(reply, "state=ENABLED") != NULL) {
		WMG_DUMP("ap status is: enable\n");
		return 0;
	} else if (strstr(reply, "state=DISABLED") != NULL) {
		WMG_DUMP("ap status is: disable\n");
		return 1;
	} else {
		WMG_DUMP("ap status is unknowd\n");
		return -1;
	}
}

__attribute__((unused)) static wmg_status_t linux_ap_cmd_reload()
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};

	sprintf(cmd, "%s", "RELOAD");
	cmd[CMD_MAX_LEN - 1] = '\0';
	if (linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to reload ap config, reply %s\n", reply);
		return WMG_STATUS_FAIL;
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t wmg_sta_device_add(dev_node_t *list_head, char *bssid)
{
	dev_node_t *new = NULL;
	int i = 0;
	char *pch = NULL;

	if (list_head == NULL) {
		WMG_ERROR("head node is NULL!\n");
		return WMG_STATUS_INVALID;
	}

	new = (dev_node_t *)malloc(sizeof(dev_node_t));
	if(new == NULL) {
		WMG_ERROR("failed to allocate memory for device node\n");
		return WMG_STATUS_FAIL;
	}
	new->next = NULL;
	memset(new->bssid, 0, (sizeof(uint8_t) * 6));
	pch = strtok(bssid, ":");
	for(;((pch != NULL) && (i < 6)); i++) {
		new->bssid[i] = char2uint8(pch);
		pch = strtok(NULL, ":");
	}
	new->next = list_head->next;
	list_head->next = new;

	return WMG_STATUS_SUCCESS;
}

static int wmg_sta_device_remove(dev_node_t *list_head, char *bssid_char)
{
	int ret = -1;
	dev_node_t *pre = list_head;
	dev_node_t *p = list_head->next;
	uint8_t bssid[6] = {0};
	int i = 0;
	char *pch;

	pch = strtok(bssid_char, ":");
	for(; (pch != NULL) && (i < 6); i++) {
		bssid[i] = char2uint8(pch);
		pch = strtok(NULL, ":");
	}

	while (p != NULL) {
		if(!memcmp(p->bssid, bssid, (sizeof(uint8_t) * 6))){
			WMG_DUMP("remove device '%s'\n", bssid_char);
			pre->next = p->next;
			free(p);
			ret = 0;
			break;
		}
		pre = p;
		p = p->next;
	}

	if (ret == -1)
		WMG_WARNG("%s is not found, remove device failed\n");

	return ret;
}

static void wmg_sta_device_print(dev_node_t *list_head)
{
	dev_node_t *p = list_head->next;
	int i = 1;

	while (p != NULL) {
		WMG_DUMP("the %dth device is '%x%x:%x%x:%x%x:%x%x:%x%x:%x%x'\n", i,
								(p->bssid[0] >> 4),
								(p->bssid[0] & 0xf),
								(p->bssid[1] >> 4),
								(p->bssid[1] & 0xf),
								(p->bssid[2] >> 4),
								(p->bssid[2] & 0xf),
								(p->bssid[3] >> 4),
								(p->bssid[3] & 0xf),
								(p->bssid[4] >> 4),
								(p->bssid[4] & 0xf),
								(p->bssid[5] >> 4),
								(p->bssid[5] & 0xf));
		p = p->next;
		i++;
	}
}

static void ap_event_notify_to_ap_dev(wifi_ap_event_t event)
{
	if (ap_inf_object.ap_event_cb) {
		ap_inf_object.ap_event_cb(event);
	}
}

static void ap_event_notify(wifi_ap_event_t event)
{
	evt_send(ap_inf_object.ap_event_handle, event);
	ap_event_notify_to_ap_dev(event);
}

static int ap_dispatch_event(const char *event_str, int nread)
{
	int event = 0, ret = 0;
	char *event_data = NULL;

	if (!event_str || !event_str[0]) {
		WMG_WARNG("hostapd event is NULL!\n");
		return 0;
	}

	WMG_DUMP("receive ap event: '%s'\n", event_str);
	if (strstr(event_str, "AP-ENABLED")) {
		event = WIFI_AP_ENABLED;
		ap_event_notify(event);
	} else if (strstr(event_str, "AP-DISABLED")) {
		event = WIFI_AP_DISABLED;
		ap_event_notify(event);
	} else if (strstr(event_str, "AP-STA-CONNECTED")) {
		event_data = strchr(event_str, ' ');
		if (event_data) {
			event_data++;
			WMG_DUMP("bssid=%s\n", event_data);
			ret = wmg_sta_device_add(ap_inf_object.dev_list, event_data);
			if (ret == 0)
				ap_inf_object.sta_num++;
			wmg_sta_device_print(ap_inf_object.dev_list);
		}
		event = WIFI_AP_STA_CONNECTED;
		ap_event_notify(event);
	} else if (strstr(event_str, "AP-STA-DISCONNECTED")) {
		event_data = strchr(event_str, ' ');
		if (event_data) {
			event_data++;
			ret = wmg_sta_device_remove(ap_inf_object.dev_list, event_data);
			if (ret == 0)
				ap_inf_object.sta_num--;
			wmg_sta_device_print(ap_inf_object.dev_list);
		}
		event = WIFI_AP_STA_DISCONNECTED;
		ap_event_notify(event);
	} else {
		event = WIFI_AP_UNKNOWN;
	}

	return 0;
}

/* Free the whole STA list and reset its owner. Safe to call on NULL. */
static void linux_ap_free_dev_list(void)
{
	dev_node_t *node = ap_inf_object.dev_list;

	while (node != NULL) {
		dev_node_t *next = node->next;
		free(node);
		node = next;
	}
	ap_inf_object.dev_list = NULL;
	ap_inf_object.sta_num = 0;
}

/* Release everything acquired by a partially completed AP enable.
 * Runs in reverse acquisition order and does not depend on ap_enable_flag,
 * which stays false until the whole transaction succeeds. */
static void linux_ap_rollback_enable(int wpad_started, int evt_started,
				     int hapd_enabled, int network_configured)
{
	char cmd[CMD_MAX_LEN] = {0};

	if (network_configured) {
		/* Stop the DHCP server and drop the address set for this AP */
		wmg_system("pidof udhcpd | xargs kill -9 2>/dev/null", NULL, 0);
		snprintf(cmd, sizeof(cmd), "ifconfig %s 0.0.0.0",
			(get_hapd_ap_iface() == NULL) ? "wlan0" : get_hapd_ap_iface());
		wmg_system(cmd, NULL, 0);
	}

	if (hapd_enabled) {
		linux_ap_cmd_disable();
	}

	/* Stop the event producer before releasing the list it mutates */
	if (wpad_started) {
		deinit_wpad(WIFI_AP);
	}
	if (evt_started) {
		evt_socket_exit(ap_inf_object.ap_event_handle);
	}

	linux_ap_free_dev_list();
	ap_inf_object.ap_enable_flag = WMG_FALSE;
	global_ap_config.ap_config_enable = false;
}

static wmg_status_t linux_ap_enable(wifi_ap_config_t *ap_config)
{
	wmg_status_t ret = WMG_STATUS_FAIL;
	/* Only OOM is distinguished; every other failure reports FAIL */
	wmg_status_t cleanup_ret = WMG_STATUS_FAIL;
	char cmd[255] = {0};
	int wpad_started = 0;
	int evt_started = 0;
	int hapd_enabled = 0;
	int network_configured = 0;

	if(ap_inf_object.ap_enable_flag == WMG_TRUE) {
		WMG_WARNG("ap mode has been enable\n");
		return WMG_STATUS_UNHANDLED;
	}

	/* A previous failed enable may have left a list behind; never overwrite it */
	linux_ap_free_dev_list();

	init_wpad_para_t hostapd_para = {
		.wifi_mode = WIFI_AP,
		.dispatch_event = ap_dispatch_event,
		.linux_mode_private_data = ap_inf_object.ap_private_data,
	};

	if(init_wpad(hostapd_para)) {
		WMG_ERROR("init hostapd failed\n");
		return WMG_STATUS_FAIL;
	}
	wpad_started = 1;

	if(evt_socket_init(ap_inf_object.ap_event_handle)) {
		WMG_ERROR("failed to initialize linux ap event socket\n");
		goto cleanup;
	}
	evt_started = 1;
	WMG_DEBUG("init hostapd success\n");

	ap_inf_object.dev_list = (dev_node_t *)malloc(sizeof(dev_node_t));
	if (ap_inf_object.dev_list == NULL) {
		WMG_ERROR("failed to allocate memory for device list\n");
		cleanup_ret = WMG_STATUS_NOMEM;
		goto cleanup;
	}
	memset(ap_inf_object.dev_list->bssid, 0, (sizeof(uint8_t) * 6));
	ap_inf_object.dev_list->next = NULL;

	ret = linux_ap_cmd_status();
	if(ret == 1){
		WMG_DEBUG("hapd is already disable\n");
	} else if (ret == 0){
		WMG_DEBUG("hapd is already enable, disable now...\n");
		if(linux_ap_cmd_disable()){
			WMG_ERROR("reload hapd config faile\n");
			goto cleanup;
		}
		ap_inf_object.ap_enable_flag = WMG_FALSE;
	} else {
		WMG_ERROR("failed to get ap hapd status\n");
		goto cleanup;
	}

	ret = linux_ap_set_config(ap_config);
	if(ret){
		WMG_ERROR("failed to set ap hapd config\n");
		goto cleanup;
	}

	WMG_DEBUG("Enable ap hapd now...\n");
	ret = linux_ap_cmd_enable();
	if(ret){
		WMG_ERROR("failed to enable ap hapd\n");
		goto cleanup;
	}
	hapd_enabled = 1;

	sprintf(cmd, "ifconfig %s %d.%d.%d.%d", (get_hapd_ap_iface() == NULL) ? "wlan0" : get_hapd_ap_iface(),
			global_ap_config.ap_config->ip_addr[0], global_ap_config.ap_config->ip_addr[1],
			global_ap_config.ap_config->ip_addr[2], global_ap_config.ap_config->ip_addr[3]);
	wmg_system(cmd, NULL, 0);
	/* Means "network resources acquired and needing undo", not "network fully
	 * ready". ifconfig above already assigned the AP address, so a later
	 * udhcpd start failure must still go through this rollback. */
	network_configured = 1;

	/* softap DHCP server — udhcpd is embedded in busybox, lighter than dnsmasq
	 * (dnsmasq also does DNS caching which is unused in softap config-net scenario) */
	sprintf(cmd, "udhcpd %s/udhcpd.conf", WMG_CONFIG_PATH);
	wmg_system(cmd, NULL, 0);

	if (check_process_is_exist("udhcpd", 7)) {
		WMG_DEBUG("start udhcpd success\n");
	} else {
		sleep(1);
		if (!check_process_is_exist("udhcpd", 7)) {
			WMG_ERROR("start udhcpd failed\n");
			goto cleanup;
		}
	}
	ap_inf_object.ap_enable_flag = WMG_TRUE;

	return WMG_STATUS_SUCCESS;

cleanup:
	/* Do not fall back to ret here: in the "hapd already enabled" branch it
	 * holds linux_ap_cmd_status()'s 0, which is WMG_STATUS_SUCCESS. */
	WMG_ERROR("ap enable failed, rolling back acquired resources\n");
	linux_ap_rollback_enable(wpad_started, evt_started, hapd_enabled, network_configured);
	return cleanup_ret;
}

static wmg_status_t linux_ap_disable()
{
	wmg_status_t ret;
	char cmd[CMD_MAX_LEN] = {0};

	if(ap_inf_object.ap_enable_flag == WMG_FALSE) {
		WMG_WARNG("ap mode has been disable\n");
		return WMG_STATUS_UNHANDLED;
	}

	ret = linux_ap_cmd_status();
	if(ret == 1){
		WMG_DEBUG("hapd is already disable, not need to send disable cmd\n");
	} else {
		ret = linux_ap_cmd_disable();
		if(ret){
			WMG_ERROR("failed to disable ap hapd\n");
			return WMG_STATUS_FAIL;
		}
	}

	WMG_INFO("linux hostapd stop now\n");
	/* Stop the event producer before releasing the list it may still
	 * mutate, then clear the owner so a later re-enable won't touch
	 * freed nodes (regression 2.1). */
	if (ap_inf_object.ap_enable_flag == WMG_TRUE) {
		if (deinit_wpad(WIFI_AP) == WMG_STATUS_SUCCESS) {
			evt_socket_exit(ap_inf_object.ap_event_handle);
		}
	} else {
		WMG_INFO("linux hostapd already has stop\n");
	}

	linux_ap_free_dev_list();

	sprintf(cmd, "ifconfig %s 0.0.0.0", (get_hapd_ap_iface() == NULL) ? "wlan0" : get_hapd_ap_iface());
	wmg_system(cmd, NULL, 0);
	wmg_system("rm -rf /var/lib/misc/", NULL, 0);
	wmg_system("pidof udhcpd | xargs kill -9 2>/dev/null", NULL, 0);

	ap_inf_object.ap_enable_flag = WMG_FALSE;

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_ap_get_config(wifi_ap_config_t *ap_config)
{
	WMG_DEBUG("ap get config\n");

	int count = 0;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};

	sprintf(cmd, "%s", "STATUS");
	cmd[CMD_MAX_LEN - 1] = '\0';
	if (linux_command_to_hostapd(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to get ap status\n");
		return WMG_STATUS_FAIL;
	}
	if (strstr(reply, "state=DISABLED") != NULL) {
		WMG_DEBUG("hapd is disabled, please enable first\n");
		return WMG_STATUS_FAIL;
	}

	if (parse_ap_config(reply, ap_config)) {
		WMG_ERROR("failed to parse ap config\n");
		return WMG_STATUS_FAIL;
	}

	if (global_ap_config.ap_config_enable != true) {
		WMG_ERROR("failed to get ap config psk\n");
		return WMG_STATUS_FAIL;
	} else {
		strcpy(ap_config->psk, global_ap_config.ap_config->psk);
		ap_config->sec = global_ap_config.ap_config->sec;
		memcpy(ap_config->ip_addr, global_ap_config.ap_config->ip_addr, 4);
		memcpy(ap_config->gw_addr, global_ap_config.ap_config->gw_addr, 4);
	}

	/* A rolled-back enable leaves dev_list NULL */
	dev_node_t *p = (ap_inf_object.dev_list != NULL) ? ap_inf_object.dev_list->next : NULL;
	while (p != NULL) {
		memcpy(ap_config->dev_list[count].sta_mac, p->bssid, (sizeof(uint8_t) * 6));
		p = p->next;
		count++;
		if (count >= ap_config->sta_dev_list_num)
		break;
	}
	WMG_DUMP("number of connected sta is %d\n", count);

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_platform_extension(int cmd, void* cmd_para,int *erro_code)
{
	switch (cmd) {
		case AP_CMD_ENABLE:
			return linux_ap_enable((wifi_ap_config_t *)cmd_para);
		case AP_CMD_DISABLE:
			return linux_ap_disable();
		case AP_CMD_GET_CONFIG:
			return linux_ap_get_config((wifi_ap_config_t *)cmd_para);
		case AP_CMD_SET_SCAN_PARAM:
			return WMG_STATUS_FAIL;
		case AP_CMD_GET_SCAN_RESULTS:
			WMG_INFO("linux ap mode unsupport get scan results\n");
			return WMG_STATUS_FAIL;
		default:
			return WMG_STATUS_FAIL;
	}
}

static wmg_status_t linux_ap_inf_init(ap_event_cb_t ap_event_cb)
{
	if(ap_inf_object.ap_init_flag == WMG_FALSE) {
		WMG_INFO("linux ap init now\n");
		ap_inf_object.ap_event_handle = (event_handle_t *)malloc(sizeof(event_handle_t));
		if(ap_inf_object.ap_event_handle != NULL) {
			memset(ap_inf_object.ap_event_handle, 0, sizeof(event_handle_t));
			ap_inf_object.ap_event_handle->evt_socket[0] = -1;
			ap_inf_object.ap_event_handle->evt_socket[1] = -1;
			ap_inf_object.ap_event_handle->evt_socket_enable = WMG_FALSE;
		} else {
			WMG_ERROR("failed to allocate memory for linux hapd event_handle\n");
			goto event_erro;
		}
		if(ap_event_cb != NULL){
			ap_inf_object.ap_event_cb = ap_event_cb;
		}
		ap_inf_object.ap_init_flag = WMG_TRUE;
	} else {
		WMG_INFO("linux hostapd already init\n");
	}
	return WMG_STATUS_SUCCESS;

event_erro:
	free(ap_inf_object.dev_list);
	return WMG_STATUS_FAIL;
}

static wmg_status_t linux_ap_inf_deinit(void *p)
{
	if(ap_inf_object.ap_init_flag == WMG_TRUE) {
		if(ap_inf_object.ap_event_handle != NULL) {
			free(ap_inf_object.ap_event_handle);
			ap_inf_object.ap_event_cb = NULL;
		}
		ap_inf_object.ap_init_flag = WMG_FALSE;
	} else {
		WMG_INFO("linux hostapd already init\n");
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_ap_inf_enable()
{
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_ap_inf_disable()
{
	linux_ap_disable();
	return WMG_STATUS_SUCCESS;
}

static wmg_ap_inf_object_t ap_inf_object = {
	.ap_init_flag = WMG_FALSE,
	.ap_enable_flag = WMG_FALSE,
	.sta_num = 0,
	.ap_event_cb = NULL,
	.ap_event_handle = NULL,
	.ap_private_data = NULL,

	.ap_inf_init = linux_ap_inf_init,
	.ap_inf_deinit = linux_ap_inf_deinit,
	.ap_inf_enable = linux_ap_inf_enable,
	.ap_inf_disable = linux_ap_inf_disable,
	.ap_platform_extension = linux_platform_extension,
};

wmg_ap_inf_object_t * ap_linux_inf_object_register(void)
{
	return &ap_inf_object;
}
