#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <wpa_ctrl.h>
#include <linux_sta.h>
#include <udhcpc.h>
#include <utils.h>
#include <wifi_log.h>
#include <unistd.h>
#include <wifimg.h>
#include <wmg_sta.h>
#include <linux/event.h>
#include <linux/udhcpc.h>
#include <linux/scan.h>
#include <linux_common.h>
#include <sys/times.h>
#include <linux_get_config.h>

#define DHCP_UPDATE_SECONDS 600

static wmg_sta_inf_object_t sta_inf_object;

#define LIST_ENTRY_NUME_MAX 64
#define NET_ID_LEN 10

typedef enum {
	WIFI_WPA_UNKNOWN,
	WIFI_WPA_DISCONNECTED,
	WIFI_WPA_COMPLETED,
	WIFI_WPA_SCANNING,
} wifi_wpa_state_t;

typedef struct {
	wifi_wpa_state_t wpa_state;
	char id[8];
	char freq[16];
	char bssid[18];
	char ssid[SSID_MAX_LEN + 1];
	char address[18];
	char ip_address[16];
	char pairwise_cipher[64];
	char group_cipher[64];
	char key_mgmt[32];
} wifi_wpa_status_t;

typedef struct {
	/* Can only be updated during Phase udpcpc */
	char last_ssid[SSID_MAX_LEN + 1];
	char last_bssid[18];
	int last_time;
	uint8_t ip_need;
} linux_sta_private_data_t;

const char *wmg_sta_event_to_str(wifi_sta_event_t event)
{
	switch (event) {
	case WIFI_DISCONNECTED:
		return "DISCONNECTED";
	case WIFI_SCAN_STARTED:
		return "SCAN_STARTED";
	case WIFI_SCAN_FAILED:
		return "SCAN_FAILED";
	case WIFI_SCAN_RESULTS:
		return "SCAN_RESULTS";
	case WIFI_NETWORK_NOT_FOUND:
		return "NETWORK_NOT_FOUND";
	case WIFI_PASSWORD_INCORRECT:
		return "PASSWORD_INCORRECT";
	case WIFI_ASSOC_REJECT:
		return "ASSOC_REJECT";
	case WIFI_CONNECTED:
		return "CONNECTED";
	case WIFI_TERMINATING:
		return "TERMINATING";
	default:
		return "UNKNOWN";
	}
}

/* Sometimes we only need whether it is connected, and do not need the specific information of the connection */
/* When wpa_status is NULL, no specific information is output */
static wmg_status_t wpa_parse_status(char *status_buf, wifi_wpa_status_t *wpa_status)
{
	char *pos = NULL, *pch = NULL;
	char utf8_tmp_buf[SSID_MAX_LEN * 4] = {0};

	if (status_buf == NULL || wpa_status == NULL) {
		WMG_ERROR("invalid parameters\n");
		return WMG_STATUS_INVALID;
	}

	pos = strstr(status_buf, "wpa_state");
	if (pos != NULL) {
		pos += 10;
		if (strncmp(pos, "COMPLETED", 9) != 0) {
			WMG_WARNG("Warning: wpa state is inactive\n");
			return WMG_STATUS_FAIL;
		} else {
			WMG_DEBUG("wpa state is completed\n");
			if(wpa_status != NULL) {
				wpa_status->wpa_state = WIFI_WPA_COMPLETED;
				pch = strtok(status_buf, "'\n'");
				while (pch != NULL) {
					if (strncmp(pch, "id=", 3) == 0) {
						strcpy(wpa_status->id, (pch + 3));
						WMG_DEBUG("%s\n", wpa_status->id);
					}
					if (strncmp(pch, "freq=", 5) == 0) {
						strcpy(wpa_status->freq, (pch + 5));
						WMG_DEBUG("%s\n", wpa_status->freq);
					}
					if (strncmp(pch, "bssid=", 6) == 0) {
						strcpy(wpa_status->bssid, (pch + 6));
						WMG_DEBUG("%s\n", wpa_status->bssid);
					}
					if (strncmp(pch, "ssid=", 5) == 0) {
						memset(utf8_tmp_buf, '\0', SSID_MAX_LEN * 4);
						if(((strlen(pch) - 5) <= (SSID_MAX_LEN * 4)) &&
							(convert_utf8((pch + 5), utf8_tmp_buf) <= SSID_MAX_LEN)){
							strcpy(wpa_status->ssid, utf8_tmp_buf);
							WMG_DEBUG("%s\n", wpa_status->ssid);
						} else {
							WMG_DEBUG("ssid is too long(%d)\n", (strlen(pch) - 5));
							return WMG_STATUS_INVALID;
						}
					}
					if (strncmp(pch, "address=", 8) == 0) {
						strcpy(wpa_status->address, (pch + 8));
						WMG_DEBUG("%s\n", wpa_status->address);
					}
					if (strncmp(pch, "ip_address=", 11) == 0) {
						strcpy(wpa_status->ip_address, (pch + 11));
						WMG_DEBUG("%s\n", wpa_status->ip_address);
					}
					if (strncmp(pch, "pairwise_cipher=", 16) == 0) {
						strcpy(wpa_status->pairwise_cipher, (pch + 16));
						WMG_DEBUG("%s\n", wpa_status->pairwise_cipher);
					}
					if (strncmp(pch, "group_cipher=", 13) == 0) {
						strcpy(wpa_status->group_cipher, (pch + 13));
						WMG_DEBUG("%s\n", wpa_status->group_cipher);
					}
					if (strncmp(pch, "key_mgmt=", 9) == 0) {
						strcpy(wpa_status->key_mgmt, (pch + 9));
						WMG_DEBUG("%s\n", wpa_status->key_mgmt);
					}
					pch = strtok(NULL, "'\n'");
				}
			}
			return WMG_STATUS_SUCCESS;
		}
	} else {
		WMG_ERROR("status info is invalid\n");
		return WMG_STATUS_INVALID;
	}
}

static wmg_status_t wpa_parse_signal_info(char *status, wifi_sta_info_t *sta_info)
{
	char rssi[32] = {0};
	char *pch;

	if (status == NULL || sta_info == NULL) {
		WMG_ERROR("invalid parameters\n");
		return WMG_STATUS_INVALID;
	}

	sta_info->rssi = 0;

	pch = strtok(status, "'\n'");
	while (pch != NULL) {
		if (strncmp(pch, "RSSI=", 5) == 0) {
			strcpy(rssi, (pch + 5));
			WMG_DEBUG("RSSI=%s\n", rssi);
			break;
		}
		pch = strtok(NULL, "'\n'");
	}
	sta_info->rssi = atoi(rssi);
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_command_to_supplicant(char const *cmd, char *reply, size_t reply_len)
{
	return command_to_wpad(cmd, reply, reply_len, WIFI_STATION, NULL);
}

/**
 * compared network in wpa_supplicant
 * return bitmap;
 * bitmap[0] : if exist?
 * bitmap[1] : if connected?
 * 0b000(0): network not exist, or error
 * 0b001(1): network exist, not connected, information does not match
 * 0b011(3): network exist, connected, information does not match
 * 0b101(5): network exist, not connected, information match
 * 0b111(7): network exist, connected, information match
 */
#define NETWORK_BP_EXIS          0b001
#define NETWORK_BP_CONET         0b010
#define NETWORK_BP_IFOR_MATCH    0b100
#define MAX_LIST_NUM 20
/* if information not match, we will remove the network */
/* if information match, we will return valid netid */
static uint8_t wpa_network_comp(const char *ssid, const char *psk, wifi_sec key_type, int *netid)
{
	int i = 0;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[REPLY_BUF_SIZE] = {0};
	/* Keep one extra slot as the NULL sentinel used by the loops below */
	char *pch_entry_p[LIST_ENTRY_NUME_MAX + 1] = {0};
	char *pch_entry = NULL;
	char *pch_item = NULL;
	char *delim_entry = "'\n'";
	char *delim_item = "'\t'";
	wifi_sta_list_nod_t wifi_sta_list_nod[MAX_LIST_NUM] = {0};
	char utf8_tmp_buf[SSID_MAX_LEN * 4] = {0};
	int list_num = 0;
	char key_mgmt_reply[32] = {0}, proto_reply[32] = {0};
	uint8_t ret_flag = 0;

	if (!ssid || !ssid[0]) {
		WMG_WARNG("ssid is NULL!\n");
		return 0;
	}

	sprintf(cmd, "%s", "LIST_NETWORKS");
	cmd[CMD_MAX_LEN - 1] = '\0';
	if (linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_WARNG("do list networks error!\n");
		return 0;
	}

	if(strlen(reply) < 34){
		WMG_DEBUG("Here has no entry save\n");
		return 0;
	}

	pch_entry = strtok((reply + 34), delim_entry);
	while((pch_entry != NULL) && (i < MAX_LIST_NUM)) {
		pch_entry_p[i] = pch_entry;
		i++;
		pch_entry = strtok(NULL, delim_entry);
	}

	list_num = i;
	if(list_num >= MAX_LIST_NUM) {
		WMG_WARNG("wpa_supplicant list num >= %d\n", MAX_LIST_NUM);
	} else {
		WMG_DEBUG("wpa_supplicant list num is:%d\n", list_num);
	}

	memset(wifi_sta_list_nod, 0, sizeof(wifi_sta_list_nod_t) * MAX_LIST_NUM);
	for(i = 0; pch_entry_p[i] != NULL; i++) {
		/* get network id */
		pch_item = strtok(pch_entry_p[i], delim_item);
		if(pch_item != NULL) {
			wifi_sta_list_nod[i].id = atoi(pch_item);
		}

		/* get network ssid */
		pch_item = strtok(NULL, delim_item);
		memset(utf8_tmp_buf, '\0', SSID_MAX_LEN * 4);
		if((pch_item != NULL) && (strlen(pch_item) <= (SSID_MAX_LEN * 4)) &&
				(convert_utf8(pch_item, utf8_tmp_buf) <= SSID_MAX_LEN)){
			strcpy((wifi_sta_list_nod[i].ssid), utf8_tmp_buf);
		} else {
			strcpy((wifi_sta_list_nod[i].ssid), "NULL");
			WMG_WARNG("An illegal ssid was obtained and has been set to NULL\n");
		}
		if(!strcmp(ssid, wifi_sta_list_nod[i].ssid)) {
			ret_flag |= NETWORK_BP_EXIS;
			WMG_DEBUG("--wpa_supplicant network table has the ssid(%s)--\n", wifi_sta_list_nod[i].ssid);
		} else {
			continue;
		}

		/* get network bssid */
		pch_item = strtok(NULL, delim_item);

		/* get network flag */
		pch_item = strtok(NULL, delim_item);
		if(pch_item != NULL) {
			if(strstr(pch_item, "CURRENT")) {
				ret_flag |= NETWORK_BP_CONET;
				WMG_DEBUG("--wpa_supplicant network table has the ssid(%s)(CURRENT)--\n", wifi_sta_list_nod[i].ssid);
			}
		}

		/* When the execution reaches this point
		 * and the required information has been obtained
		 * jump out of the loop.
		 */
		break;
	}

	/* network exis, compared connect information now */
	if(ret_flag & NETWORK_BP_EXIS) {
		/* get key_mgmt */
		sprintf(cmd, "GET_NETWORK %d key_mgmt", wifi_sta_list_nod[i].id);
		if(linux_command_to_supplicant(cmd, key_mgmt_reply, 32)) {
			WMG_ERROR("get network %d key_mgmt fail!, remove now\n", wifi_sta_list_nod[i].id);
			goto error_remove_network;
		}

		if((key_type == WIFI_SEC_WPA_PSK) || (key_type == WIFI_SEC_WPA2_PSK) || (key_type == WIFI_SEC_WPA2_PSK_SHA256)) {
			/* get proto */
			sprintf(cmd, "GET_NETWORK %d proto", wifi_sta_list_nod[i].id);
			if(linux_command_to_supplicant(cmd, proto_reply, 32)) {
				WMG_ERROR("get network %d proto fail!, remove now\n", wifi_sta_list_nod[i].id);
				goto error_remove_network;
			}
		}

		switch (key_type) {
			case WIFI_SEC_NONE:
				if(!strcmp(key_mgmt_reply, "NONE")) {
					ret_flag |= NETWORK_BP_IFOR_MATCH;
				}
			case WIFI_SEC_WEP:
				/*connection strategy: if we are connected to route wep
				 * we need to reconnect because route wep may not be able to obtain dhcp.*/
				if(ret_flag & NETWORK_BP_CONET) {
					goto error_remove_network;
				} else {
					if(!strcmp(key_mgmt_reply, "NONE")) {
						ret_flag |= NETWORK_BP_IFOR_MATCH;
					}
				}
			break;
			case WIFI_SEC_WPA_PSK:
				if(!strcmp(proto_reply, "WPA") && !strcmp(key_mgmt_reply, "WPA-PSK")) {
					ret_flag |= NETWORK_BP_IFOR_MATCH;
				}
			break;
			case WIFI_SEC_WPA2_PSK:
				if((!strcmp(proto_reply, "WPA2") || !strcmp(proto_reply, "RSN")) && !strcmp(key_mgmt_reply, "WPA-PSK")) {
					ret_flag |= NETWORK_BP_IFOR_MATCH;
				}
			case WIFI_SEC_WPA2_PSK_SHA256:
				if((!strcmp(proto_reply, "WPA2") || !strcmp(proto_reply, "RSN")) && !strcmp(key_mgmt_reply, "WPA-PSK-SHA256")) {
					ret_flag |= NETWORK_BP_IFOR_MATCH;
				}
			break;
			case WIFI_SEC_WPA3_PSK:
				if(!strcmp(key_mgmt_reply, "SAE")) {
					ret_flag |= NETWORK_BP_IFOR_MATCH;
				}
			break;
		}
		if(ret_flag & NETWORK_BP_IFOR_MATCH) {
			WMG_INFO("There is a matching network(%d:%s) in wpa_supplicant\n", wifi_sta_list_nod[i].id, ssid);
			*netid = wifi_sta_list_nod[i].id;
			return ret_flag;
		} else {
			WMG_WARNG("old network %d (%s) information not match, remove now\n", wifi_sta_list_nod[i].id);
			goto error_remove_network;
		}
	}

	return ret_flag;

error_remove_network:
	sprintf(cmd, "REMOVE_NETWORK %d", wifi_sta_list_nod[i].id);
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("remove network %d fail!\n", wifi_sta_list_nod[i].id);
		*netid = -1;
	}
	return 0;
}

/*
 * ssid to netid
 * return@ -1: has no ssid
 * return@ 0: has ssid but not connected
 * return@ 1: has ssid and connected
 */
static int wpa_conf_ssid2netid(const char *ssid, int *net_id)
{
	int i = 0;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[REPLY_BUF_SIZE] = {0};
	char utf8_tmp_buf[SSID_MAX_LEN * 4] = {0};
	/* Keep one extra slot as the NULL sentinel used by the loops below */
	char *pch_entry_p[LIST_ENTRY_NUME_MAX + 1] = {0};
	char *pch_entry = NULL;
	char *pch_item = NULL;
	char *delim_entry = "'\n'";
	char *delim_item = "'\t'";
	int network_id = -1;
	int exist_state = -1;

	if (ssid == NULL) {
		WMG_ERROR("invalid parameters\n");
		return exist_state;
	}

	sprintf(cmd, "%s", "LIST_NETWORKS");
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to list networks, reply %s\n", reply);
		return exist_state;
	}

	if(strlen(reply) < 34){
		WMG_INFO("Here has no entry save\n");
		return exist_state;
	}

	pch_entry = strtok((reply + 34), delim_entry);
	while(pch_entry != NULL && i < LIST_ENTRY_NUME_MAX) {
		pch_entry_p[i] = pch_entry;
		i++;
		pch_entry = strtok(NULL, delim_entry);
	}

	WMG_DEBUG("sys save num is:%d\n", i);

	for(i = 0; pch_entry_p[i] != NULL; i++) {
		pch_item = strtok(pch_entry_p[i], delim_item);
		if(pch_item != NULL) {
			network_id = atoi(pch_item);
		}
		pch_item = strtok(NULL, delim_item);
		if(pch_item != NULL) {
			memset(utf8_tmp_buf, '\0', SSID_MAX_LEN * 4);
			if((strlen(pch_item) <= (SSID_MAX_LEN * 4)) &&
				(convert_utf8((pch_item), utf8_tmp_buf) <= SSID_MAX_LEN)){
				if(strcmp(ssid, utf8_tmp_buf) == 0){
					*net_id = network_id;
					exist_state = 0;
					pch_item = strtok(NULL, delim_item);
					pch_item = strtok(NULL, delim_item);
					if(pch_item != NULL) {
						if(strcmp("[CURRENT]", pch_item) == 0){
							exist_state = 1;
						}
					}
					break;
				}
			}
		}
	}

	if(exist_state >= 0) {
		WMG_DEBUG("%s net_id is:%d\n", ssid, *net_id);
	}
	return exist_state;
}

/* only in memory enable or disable all networks, will not save to wpa_supplicant.conf */
static wmg_status_t wpa_enable_or_disable_all_networks(wmg_bool_t enable)
{
	int i = 0;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[REPLY_BUF_SIZE] = {0};
	/* Keep one extra slot as the NULL sentinel used by the loops below */
	char *pch_entry_p[LIST_ENTRY_NUME_MAX + 1] = {0};
	char *pch_entry = NULL;
	char *pch_item = NULL;
	char *delim_entry = "'\n'";
	char *delim_item = "'\t'";
	int network_id = -1;
	int priority = 0;

	sprintf(cmd, "%s", "LIST_NETWORKS");
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("do list networks error\n");
		return WMG_STATUS_FAIL;
	}

	if(strlen(reply) < 34){
		WMG_INFO("Here has no entry save\n");
		return WMG_STATUS_FAIL;
	}

	pch_entry = strtok((reply + 34), delim_entry);
	while(pch_entry != NULL && i < LIST_ENTRY_NUME_MAX) {
		pch_entry_p[i] = pch_entry;
		i++;
		pch_entry = strtok(NULL, delim_entry);
	}

	for(i = 0, priority = 0; pch_entry_p[i] != NULL; i++) {
		pch_item = strtok(pch_entry_p[i], delim_item);
		/* get network id */
		if(pch_item != NULL) {
			network_id = atoi(pch_item);
		}
		/* get network ssid */
		pch_item = strtok(NULL, delim_item);
		/* get network bssid */
		pch_item = strtok(NULL, delim_item);
		/* get network flag */
		pch_item = strtok(NULL, delim_item);
		if((pch_item != NULL) && (strstr(pch_item, "[CURRENT]") != NULL)) {
			/* set current connection network  priority to 1 */
			priority = 1;
		}

		sprintf(cmd, "SET_NETWORK %d priority %d", network_id, priority);
		if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
			WMG_INFO("failed to set network %d priority %d, reply %s\n", network_id, priority, reply);
			return WMG_STATUS_FAIL;
		}

		sprintf(cmd, "SET_NETWORK %d disabled %d", network_id, enable ? 0 : 1);
		if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
			WMG_WARNG("failed to set network %d disabled %d, reply %s\n", network_id, enable ? 0 : 1, reply);
			return WMG_STATUS_FAIL;
		}
	}
	return WMG_STATUS_SUCCESS;
}

/* enable all networks and save to wpa_supplicant.conf */
static wmg_status_t wpa_conf_enable_or_disable_all_networks(wmg_bool_t enable)
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[REPLY_BUF_SIZE] = {0};

	wpa_enable_or_disable_all_networks(enable);

	/* save config */
	sprintf(cmd, "%s", "SAVE_CONFIG");
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to save config to wpa_supplicant.conf\n");
		return WMG_STATUS_FAIL;
	} else {
		WMG_DUMP("save config to wpa_supplicant.conf success\n");
	}

	return WMG_STATUS_SUCCESS;
}

static int check_wpa_password(const char *passwd)
{
	int ret = -1, i;

	if (!passwd || *passwd =='\0') {
		WMG_ERROR("password is NULL\n");
		return ret;
	}

	for (i = 0; passwd[i] !='\0'; i++) {
		/* non printable char */
		if ((passwd[i] < 32) || (passwd[i] > 126)) {
			ret = -1;
			break;
		}
	}

	if (passwd[i] == '\0') {
		ret = 0;
	}

	return ret;
}

static wmg_status_t linux_sta_cmd_disconnect()
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};

	sprintf(cmd, "%s", "DISCONNECT");
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to disconnect ap, reply %s\n", reply);
		return WMG_STATUS_FAIL;
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_sta_cmd_reconnect()
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};

	sprintf(cmd, "%s", "RECONNECT");
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to reconnect ap, reply %s\n", reply);
		return WMG_STATUS_FAIL;
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_sta_cmd_status(char *reply_buf)
{
	char cmd[CMD_MAX_LEN] = {0};

	WMG_DUMP("wpa status\n");
	sprintf(cmd, "%s", "STATUS");
	if(linux_command_to_supplicant(cmd, reply_buf, EVENT_BUF_SIZE)) {
		WMG_ERROR("failed to get sta status\n");
		return WMG_STATUS_FAIL;
	} else {
		WMG_DEBUG("success to get sta status\n");
		return WMG_STATUS_SUCCESS;
	}
}

static int linux_sta_cmd_state()
{
	wmg_status_t ret;
	char reply[EVENT_BUF_SIZE] = {0};

	ret = linux_sta_cmd_status(reply);
	if(ret != WMG_STATUS_SUCCESS) {
		return -1;
	}

	if (strstr(reply, "wpa_state=COMPLETED") != NULL) {
		WMG_DUMP("sta status is: connection\n");
		return 0;
	} else if (strstr(reply, "wpa_state=DISCONNECTED") != NULL) {
		WMG_DUMP("sta status is: disconnect\n");
		return 1;
	} else if (strstr(reply, "wpa_state=SCANNING") != NULL) {
		WMG_DUMP("sta status is: scanning\n");
		return 2;
	} else {
		WMG_DUMP("ap status is unknowd\n");
		return -1;
	}
}

/*
 * wep_auth_flag(0): open
 * wep_auth_flag(1): shared
 * */
static wmg_status_t wpas_select_network(int *netid, wifi_sta_cn_para_t *cn_para, uint8_t wep_auth_flag)
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	char netid_reply[16];
	int passwd_len = 0;
	uint8_t bssid[6] = {0};

	if(*netid == -1) {
		sprintf(cmd, "%s", "ADD_NETWORK");
		if(linux_command_to_supplicant(cmd, netid_reply, 16)) {
			WMG_ERROR("failed to add network, reply %s\n", netid_reply);
			return WMG_STATUS_FAIL;
		} else {
			*netid = atoi(netid_reply);
		}

		WMG_DEBUG("add network id=%d\n", *netid);

		if(cn_para->ssid != NULL) {
			sprintf(cmd, "SET_NETWORK %d ssid \"%s\"", *netid, cn_para->ssid);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network ssid '%s', reply %s\n", cn_para->ssid, reply);
				goto err_remvo_netid;
			}
		}

		if(memcmp(cn_para->bssid, bssid, 6)) {
			sprintf(cmd, "SET_NETWORK %d bssid %02x:%02x:%02x:%02x:%02x:%02x", *netid,
					(unsigned char)(cn_para->bssid[0]), (unsigned char)(cn_para->bssid[1]),
					(unsigned char)(cn_para->bssid[2]), (unsigned char)(cn_para->bssid[3]),
					(unsigned char)(cn_para->bssid[4]), (unsigned char)(cn_para->bssid[5]));
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network bssid %02x:%02x:%02x:%02x:%02x:%02x, reply %s\n",
					(unsigned char)(cn_para->bssid[0]), (unsigned char)(cn_para->bssid[1]),
					(unsigned char)(cn_para->bssid[2]), (unsigned char)(cn_para->bssid[3]),
					(unsigned char)(cn_para->bssid[4]), (unsigned char)(cn_para->bssid[5]), reply);
				goto err_remvo_netid;
			}
		}

		switch (cn_para->sec) {
		case WIFI_SEC_NONE:
			sprintf(cmd, "SET_NETWORK %d key_mgmt NONE", *netid);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network key_mgmt(NONE), reply %s\n", reply);
				goto err_remvo_netid;
			}
			break;
		case WIFI_SEC_WPA_PSK:
			sprintf(cmd, "SET_NETWORK %d proto WPA", *netid);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network proto WPA, reply %s\n", reply);
				goto err_remvo_netid;
			}

			sprintf(cmd, "SET_NETWORK %d key_mgmt WPA-PSK", *netid);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network key_mgmt(WPA-PSK), reply %s\n", reply);
				goto err_remvo_netid;
			}

			/* check password */
			if(check_wpa_password(cn_para->password)) {
				WMG_ERROR("password is invalid\n");
				goto err_remvo_netid;
			}
			sprintf(cmd, "SET_NETWORK %d psk \"%s\"", *netid, cn_para->password);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network psk, reply %s\n", reply);
				goto err_remvo_netid;
			}
			break;
		case WIFI_SEC_WPA2_PSK:
		case WIFI_SEC_WPA2_PSK_SHA256:
			sprintf(cmd, "SET_NETWORK %d proto WPA2", *netid);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network proto WPA2, reply %s\n", reply);
				goto err_remvo_netid;
			}

			if(cn_para->sec == WIFI_SEC_WPA2_PSK) {
				sprintf(cmd, "SET_NETWORK %d key_mgmt WPA-PSK", *netid);
				if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
					WMG_ERROR("failed to set network key_mgmt(WPA-PSK), reply %s\n", reply);
					goto err_remvo_netid;
				}
			} else {
				sprintf(cmd, "SET_NETWORK %d key_mgmt WPA-PSK-SHA256", *netid);
				if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
					WMG_ERROR("failed to set network key_mgmt(WPA-PSK-SHA256), reply %s\n", reply);
					goto err_remvo_netid;
				}

				sprintf(cmd, "SET_NETWORK %d ieee80211w 2", *netid);
				if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
					WMG_ERROR("failed to set network ieee80211w(2), reply %s\n", reply);
					goto err_remvo_netid;
				}
			}

			/* check password */
			if(check_wpa_password(cn_para->password)) {
				WMG_ERROR("password is invalid\n");
				goto err_remvo_netid;
			}
			sprintf(cmd, "SET_NETWORK %d psk \"%s\"", *netid, cn_para->password);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network psk, reply %s\n", reply);
				goto err_remvo_netid;
			}
			break;
		case WIFI_SEC_WPA3_PSK:
			sprintf(cmd, "SET_NETWORK %d key_mgmt SAE", *netid);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network key_mgmt(SAE), reply %s\n", reply);
				goto err_remvo_netid;
			}

			/* check password */
			if(check_wpa_password(cn_para->password)) {
				WMG_ERROR("password is invalid\n");
				goto err_remvo_netid;
			}
			sprintf(cmd, "SET_NETWORK %d psk \"%s\"", *netid, cn_para->password);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network psk, reply %s\n", reply);
				goto err_remvo_netid;
			}

			/* set ieee80211w */
			sprintf(cmd, "SET_NETWORK %d ieee80211w 1", *netid);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set ieee80211w 1, reply %s\n", reply);
				goto err_remvo_netid;
			}
			break;
		case WIFI_SEC_WEP:
			sprintf(cmd, "SET_NETWORK %d key_mgmt NONE", *netid);
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network key_mgmt, reply %s\n", reply);
				goto err_remvo_netid;
			}

			if(cn_para->password == NULL) {
				WMG_ERROR("password is NULL\n");
				WMG_ERROR("password is invalid\n");
				goto err_remvo_netid;
			};
			passwd_len = strlen(cn_para->password);
			if((passwd_len == 10) || (passwd_len == 26)) {
				sprintf(cmd, "SET_NETWORK %d wep_key0 %s", *netid, cn_para->password);
				WMG_DEBUG("The passwd is HEX format!\n");
			} else if((passwd_len == 5) || (passwd_len == 13)) {
				sprintf(cmd, "SET_NETWORK %d wep_key0 \"%s\"", *netid, cn_para->password);
				WMG_DEBUG("The passwd is ASCII format!\n");
			} else {
				WMG_ERROR("The password does not conform to the specification!\n");
				goto err_remvo_netid;
			}
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network wep_key0, reply %s\n", reply);
				goto err_remvo_netid;
			}
			sprintf(cmd, "SET_NETWORK %d auth_alg %s", *netid, (wep_auth_flag == 0) ? "OPEN" : "SHARED");
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network auth_alg %s, reply %s\n", (wep_auth_flag == 0) ? "OPEN" : "SHARED", reply);
				goto err_remvo_netid;
			} else {
				WMG_INFO("set network auth_alg %s, reply %s\n", (wep_auth_flag == 0) ? "OPEN" : "SHARED", reply);
			}
			break;
		default:
			WMG_ERROR("unknown key mgmt\n");
			goto err_remvo_netid;
		}

		sprintf(cmd, "SET_NETWORK %d scan_ssid 1", *netid);
		if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
			WMG_ERROR("failed to set network scan_ssid(1), reply %s\n", reply);
			goto err_remvo_netid;
		}
	/* network exists, just update psk/wep_key0 */
	} else {
		WMG_INFO("-- wpa_supplicant has a valid network id(%d), use network id(%d) to connect--\n", *netid, *netid);
		if(cn_para->sec != WIFI_SEC_WEP) {
			if(cn_para->sec != WIFI_SEC_NONE) {
				WMG_INFO("-- updated network(%d) psk --\n", *netid);
				sprintf(cmd, "SET_NETWORK %d psk \"%s\"", *netid, cn_para->password);
				if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
					WMG_ERROR("failed to set network psk, reply %s\n", reply);
					goto err_remvo_netid;
				}
			}
		} else {
			WMG_INFO("-- updated network(%d) wep_key0 --\n", *netid);
			passwd_len = strlen(cn_para->password);
			if((passwd_len == 10) || (passwd_len == 26)) {
				sprintf(cmd, "SET_NETWORK %d wep_key0 %s", *netid, cn_para->password);
				WMG_DEBUG("The passwd is HEX format!\n");
			} else if((passwd_len == 5) || (passwd_len == 13)) {
				sprintf(cmd, "SET_NETWORK %d wep_key0 \"%s\"", *netid, cn_para->password);
				WMG_DEBUG("The passwd is ASCII format!\n");
			} else {
				WMG_ERROR("The password does not conform to the specification!\n");
				goto err_remvo_netid;
			}
			if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
				WMG_ERROR("failed to set network wep_key0, reply %s\n", reply);
				goto err_remvo_netid;
			}
		}
	}

	/* Only the current connection priority is 1, and other connection priorities are 0 */
	sprintf(cmd, "SET_NETWORK %d priority 1", *netid);
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to select network %d priority 1, reply %s\n", *netid, reply);
		goto err_remvo_netid;
	}

	sprintf(cmd, "SELECT_NETWORK %d", *netid);
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to select network %d, reply %s\n", *netid, reply);
		goto err_remvo_netid;
	}

	return WMG_STATUS_SUCCESS;

err_remvo_netid:
	WMG_ERROR("select network faile, remove network now\n");
	sprintf(cmd, "REMOVE_NETWORK %d", *netid);
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("select network faile, do remove network %d error!\n", *netid);
		*netid = -1;
	}
	return WMG_STATUS_FAIL;
}

static wmg_status_t wpa_set_auto_reconn(wmg_bool_t enable)
{
	wmg_status_t ret = WMG_STATUS_FAIL;
	WMG_DEBUG("get reconnn flag:%d\n",enable);

	if(enable != sta_inf_object.sta_auto_reconn) {
		sta_inf_object.sta_auto_reconn = enable;
		set_config("wpa_sta_auto_reconnect", enable ? "enable" : "disable");
		wpa_conf_enable_or_disable_all_networks(enable);
	}

	if(enable == WMG_FALSE){
		if(linux_sta_cmd_state() == 2){
			WMG_DEBUG("send disconnet cmd\n");
			ret = linux_sta_cmd_disconnect();
			if (ret) {
				WMG_ERROR("failed to send disconnect cmd\n");
				return ret;
			}
		}
	} else {
		return linux_sta_cmd_reconnect();
	}
	return WMG_STATUS_SUCCESS;
}

static int wpa_get_scan_results(get_scan_results_para_t *sta_scan_results_para)
{
	wmg_status_t ret;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[SCAN_BUF_LEN] = {0};
	int event = -1, ret_tmp;
	int try_cnt = 0;
	int results_try_cnt = 0;

scan_once:
	evt_socket_clear(sta_inf_object.sta_event_handle);
	sprintf(cmd, "%s", "SCAN");
	cmd[CMD_MAX_LEN - 1] = '\0';
	ret_tmp = linux_command_to_supplicant(cmd, reply, sizeof(reply));
	if (ret_tmp) {
		if (strncmp(reply, "FAIL-BUSY", 9) == 0) {
			WMG_DUMP("wpa_supplicant is scanning internally\n");
			sleep(2);
			goto scan_results;
		}
		ret = WMG_STATUS_FAIL;
		goto err;
	}

	while (try_cnt < SCAN_TRY_MAX) {
		ret_tmp = evt_read(sta_inf_object.sta_event_handle, (int *)&event);
		if (ret_tmp > 0) {
			WMG_DUMP("receive wpas event '%s'\n", wmg_sta_event_to_str(event));
			if (event == WIFI_SCAN_RESULTS) {
				break;
			} else if (event == WIFI_SCAN_FAILED) {
				if (try_cnt >= 2) {
					ret = WMG_STATUS_FAIL;
					goto err;
				}
				try_cnt++;
				goto scan_once;
			} else {
				try_cnt++;
			}
		} else {
			try_cnt++;
		}
	}

	if (try_cnt == SCAN_TRY_MAX && event != WIFI_SCAN_RESULTS) {
		ret = WMG_STATUS_FAIL;
		goto err;
	}

scan_results:
	sprintf(cmd, "%s", "SCAN_RESULTS");
	cmd[CMD_MAX_LEN - 1] = '\0';
	ret_tmp = linux_command_to_supplicant(cmd, reply, sizeof(reply));
	if (ret_tmp) {
		/* Counter lives outside the label: resetting it here made the
		 * retry budget unreachable, so a persistently failing control
		 * socket blocked the synchronous action forever (WR-013). */
		results_try_cnt++;
		if (results_try_cnt >= 2) {
			WMG_ERROR("failed to get scan results after %d attempts\n",
				  results_try_cnt);
			ret = WMG_STATUS_FAIL;
			goto err;
		}
		sleep(1);
		goto scan_results;
	}

	if (strlen(reply) < SCAN_BUF_LEN) {
		reply[strlen(reply)] = '\n';
	} else {
		WMG_WARNG("scan results is too long, some results may be lost\n");
	}

	remove_slash_from_scan_results(reply);
	ret_tmp = parse_scan_results(reply, sta_scan_results_para->scan_results, sta_scan_results_para->bss_num, sta_scan_results_para->arr_size);
	if (ret_tmp) {
		WMG_ERROR("failed to parse scan results\n");
		ret = WMG_STATUS_FAIL;
	} else {
		ret = WMG_STATUS_SUCCESS;
	}

err:
	return ret;
}

static void wpas_event_notify_to_sta_dev(wifi_sta_event_t event)
{
	if (sta_inf_object.sta_event_cb) {
		sta_inf_object.sta_event_cb(event);
	}
}

static void wpas_event_notify(wifi_sta_event_t event)
{
	evt_send(sta_inf_object.sta_event_handle, event);
	wpas_event_notify_to_sta_dev(event);
}

static void try_start_udhcpc(char *inf)
{
	linux_sta_private_data_t *private_data = NULL;
	private_data = (linux_sta_private_data_t *)sta_inf_object.sta_private_data;
	int now_time = time((time_t *)NULL);
	uint8_t ip_exist_flag = 0;
	uint8_t need_start_udhcpc_flag = 0;
	char reply[EVENT_BUF_SIZE] = {0};
	wifi_wpa_status_t wifi_wpa_status;
	memset(&wifi_wpa_status, 0, sizeof(wifi_wpa_status));

	WMG_INFO("try_start_udhcpc: inf=%s, ip_need=%d\n", inf, private_data ? private_data->ip_need : -1);

	wpas_event_notify(WIFI_DHCP_START);
	//Check if any ip exists
	if(private_data->ip_need == 0) {
		ip_exist_flag = is_ip_exist(inf, IPV4_ADDR_BM | IPV6_ADDR_BM);
	} else {
		ip_exist_flag = is_ip_exist(inf, private_data->ip_need);
	}
	WMG_INFO("try_start_udhcpc: ip_exist_flag=%d, need=%d\n", ip_exist_flag, private_data->ip_need);
	/* There is no required IP */
	if(((private_data->ip_need == 0) && (ip_exist_flag == 0)) ||
		(private_data->ip_need != ip_exist_flag)){

		if((private_data->ip_need == 0) || (private_data->ip_need & IPV4_ADDR_BM)) {
			WMG_DEBUG("need start udhcpc to get ipv4\n");
			need_start_udhcpc_flag |= IPV4_ADDR_BM;
		}

		/*For the time being, udhcpc6 will be started by default only if ipv6 is specified.*/
		if(private_data->ip_need & IPV6_ADDR_BM) {
			WMG_DEBUG("need start udhcpc6 to get ipv6\n");
			need_start_udhcpc_flag |= IPV6_ADDR_BM;
		}

		//start udhcpc/udhcpc6 to get ipv4/ipv6
		if(!start_udhcpc(inf, need_start_udhcpc_flag)) {
			wpas_event_notify(WIFI_DHCP_SUCCESS);
			if(private_data->ip_need & IPV4_ADDR_BM) {
				if(!linux_sta_cmd_status(reply)) {
					if(!wpa_parse_status(reply, &wifi_wpa_status)) {
						strcpy(private_data->last_ssid, wifi_wpa_status.ssid);
						strcpy(private_data->last_bssid, wifi_wpa_status.bssid);
						private_data->last_time = now_time;
					} else {
						WMG_WARNG("failed to parse station info\n");
					}
				} else {
					WMG_WARNG("failed to get station status info\n");
				}
					WMG_INFO("get ipv4 success\n");
				}
			if(private_data->ip_need & IPV6_ADDR_BM) {
				WMG_INFO("get ipv6 success\n");
			}
		} else {
			stop_udhcpc(inf, private_data->ip_need);
			if((~ip_exist_flag & IPV4_ADDR_BM) &&
				((private_data->ip_need & IPV4_ADDR_BM) || (private_data->ip_need == 0))){
				WMG_ERROR("get ipv4 fail\n");
			}
			if((~ip_exist_flag & IPV6_ADDR_BM) &&
				((private_data->ip_need & IPV6_ADDR_BM) || (private_data->ip_need == 0))){
					WMG_ERROR("get ipv6 fail\n");
			}
			wpas_event_notify(WIFI_DHCP_TIMEOUT);
		}
	/* The required IP exists */
	} else {
		//ipv4 exists, if need to start udchcpc?
		if((private_data->ip_need == 0) || (private_data->ip_need & IPV4_ADDR_BM)) {
			if(!linux_sta_cmd_status(reply)) {
				if(wpa_parse_status(reply, &wifi_wpa_status)) {
					WMG_WARNG("failed to parse station info\n");
				}
			} else {
				WMG_WARNG("failed to get station status info\n");
			}
			if((strcmp(private_data->last_ssid, wifi_wpa_status.ssid)) ||
				(strcmp(private_data->last_bssid, wifi_wpa_status.bssid))) {
				WMG_DEBUG("old ssid(%s:%s) and new ssid(%s:%s) different, need to start udhcpc get ipv4\n",
						private_data->last_ssid, wifi_wpa_status.ssid,
						private_data->last_bssid, wifi_wpa_status.bssid);
				if(!start_udhcpc(inf, IPV4_ADDR_BM)) {
					wpas_event_notify(WIFI_DHCP_SUCCESS);
					strcpy(private_data->last_ssid, wifi_wpa_status.ssid);
					strcpy(private_data->last_bssid, wifi_wpa_status.bssid);
					private_data->last_time = now_time;
				} else {
					wpas_event_notify(WIFI_DHCP_TIMEOUT);
					stop_udhcpc(inf, IPV4_ADDR_BM);
				}
			} else {
				if(((now_time) - (private_data->last_time)) > DHCP_UPDATE_SECONDS) {
					WMG_DEBUG("It(%s) took more than %d seconds to get ipv4, need to start_udhcpc get ipv4\n",
							private_data->last_ssid, DHCP_UPDATE_SECONDS);
					if(!start_udhcpc(inf, IPV4_ADDR_BM)) {
						wpas_event_notify(WIFI_DHCP_SUCCESS);
						private_data->last_time = now_time;
					} else {
						wpas_event_notify(WIFI_DHCP_TIMEOUT);
						stop_udhcpc(inf, IPV4_ADDR_BM);
					}
				} else {
					WMG_INFO("(%s)The time to get ipv4 does not exceed %d(%d) seconds, need not to start_udhcpc get ipv4\n",
							private_data->last_ssid, DHCP_UPDATE_SECONDS, ((now_time) - (private_data->last_time)));
					wpas_event_notify(WIFI_DHCP_SUCCESS);
				}
			}
		//ipv6 exists, if need to start udchcpc6?
		} else {
			wpas_event_notify(WIFI_DHCP_SUCCESS);
		}
	}
	WMG_DEBUG("**************************try udhcpc end********************************\n");
}

static char sta_inf[] = "wlan0";
static int wpas_dispatch_event(const char *event_str, int nread)
{
	int i = 0;
	char event_nae[CMD_MAX_LEN] = {0};
	char cmd[CMD_MAX_LEN] = {0};
	// char reply[EVENT_BUF_SIZE] = {0};
	char *nae_start = NULL, *nae_end = NULL;

	if (!event_str || !event_str[0]) {
		WMG_WARNG("wpa_supplicant event is NULL!\n");
		return 0;
	}

	WMG_DUMP("receive wpa event: %s\n", event_str);
	if (strncmp(event_str, "CTRL-EVENT-", 11) != 0) {
		if (!strncmp(event_str, "WPA:", 4)) {
			if (strstr(event_str, "pre-shared key may be incorrect")){
				wpas_event_notify(WIFI_PASSWORD_INCORRECT);
				return 0;
			}
		}
		return 0;
	}

	nae_start = (char *)((unsigned long)event_str + 11);
	nae_end = strchr(nae_start, ' ');
	if (nae_end) {
		while ((nae_start < nae_end) && (i < 18)) {
			event_nae[i] = *nae_start++;
			i++;
		}
		event_nae[i] = '\0';
	} else {
		WMG_DUMP("received wpa_supplicant event with empty event nae!\n");
		return 0;
	}

	WMG_DUMP("event name:%s\n", event_nae);
	if (!strcmp(event_nae, "DISCONNECTED")) {
		wpas_event_notify(WIFI_DISCONNECTED);
		/* stop_udhcpc handles kill of DHCP client + flush of IPv4/IPv6,
		 * so no separate ifconfig or disable_ipv6 toggles needed here. */
		stop_udhcpc(sta_inf, 0);
		/* temp change! Fix send RECONNECT when connecting from A to B. */
		/*if(sta_inf_object.sta_auto_reconn == WMG_TRUE) {
			linux_sta_cmd_reconnect();
		}*/

	} else if (!strcmp(event_nae, "SCAN-STARTED")) {
		wpas_event_notify(WIFI_SCAN_STARTED);
	} else if (!strcmp(event_nae, "SCAN-RESULTS")) {
		wpas_event_notify(WIFI_SCAN_RESULTS);
	} else if (!strcmp(event_nae, "SCAN-FAILED")) {
		wpas_event_notify(WIFI_SCAN_FAILED);
	} else if (!strcmp(event_nae, "NETWORK-NOT-FOUND")) {
		wpas_event_notify(WIFI_NETWORK_NOT_FOUND);
	} else if ((!strcmp(event_nae, "AUTH-REJECT")) || (!strcmp(event_nae, "ASSOC-REJECT"))) {
		if (!strcmp(event_nae, "AUTH-REJECT")) {
			wpas_event_notify(WIFI_AUTH_REJECT);
		} else {
			wpas_event_notify(WIFI_ASSOC_REJECT);
		}
		//memset(cmd, 0, CMD_MAX_LEN);
		//sprintf(cmd, "%s", "DISCONNECT");
		//cmd[CMD_MAX_LEN - 1] = '\0';
		//ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
		//if (ret) {
		//	WMG_ERROR("failed to disconnect from ap, reply %s\n", reply);
		//}
		memset(cmd, 0, CMD_MAX_LEN);
		sprintf(cmd, "ifconfig %s 0.0.0.0", sta_inf);
		wmg_system(cmd, NULL, 0);
	} else if (!strcmp(event_nae, "CONNECTED")) {
		WMG_INFO("received wpa CONNECTED event, starting DHCP on %s\n", sta_inf);
		wpas_event_notify(WIFI_CONNECTED);
		try_start_udhcpc(sta_inf);
	} else if (!strcmp(event_nae, "TERMINATING")) {
		wpas_event_notify(WIFI_TERMINATING);
		return 1;
	} else {
		// wpas_event_notify(WIFI_UNKNOWN);  /* cause scan failed */
	}

	return 0;
}

static wmg_status_t linux_sta_inf_init(sta_event_cb_t sta_event_cb,void *p)
{
	char sta_auto_reconn_buf[10] = {0};

	if(sta_inf_object.sta_init_flag == WMG_FALSE) {
		WMG_INFO("linux sta supplicant init now\n");

		sta_inf_object.sta_event_handle = (event_handle_t *)malloc(sizeof(event_handle_t));
		if(sta_inf_object.sta_event_handle != NULL) {
			memset(sta_inf_object.sta_event_handle, 0, sizeof(event_handle_t));
			sta_inf_object.sta_event_handle->evt_socket[0] = -1;
			sta_inf_object.sta_event_handle->evt_socket[1] = -1;
			sta_inf_object.sta_event_handle->evt_socket_enable = WMG_FALSE;
		} else {
			WMG_ERROR("failed to allocate memory for linux wpa event_handle\n");
			return WMG_STATUS_FAIL;
		}
		if(sta_event_cb != NULL){
			sta_inf_object.sta_event_cb = sta_event_cb;
		}
		memset(((linux_sta_private_data_t *)(sta_inf_object.sta_private_data))->last_ssid, 0,
				SSID_MAX_LEN);
		memset(((linux_sta_private_data_t *)(sta_inf_object.sta_private_data))->last_bssid, 0,
				18);
		get_config("wpa_sta_auto_reconnect", sta_auto_reconn_buf, "enable");
		if(!strcmp("disable", sta_auto_reconn_buf)) {
			sta_inf_object.sta_auto_reconn = WMG_FALSE;
		}
		WMG_DEBUG("wpa_set_auto_reconnect: %s\n", sta_auto_reconn_buf);
		sta_inf_object.sta_init_flag = WMG_TRUE;
	} else {
		WMG_INFO("linux supplicant already init\n");
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_sta_inf_deinit(void *p)
{
	if(sta_inf_object.sta_init_flag == WMG_TRUE) {
		wmg_system("ifconfig wlan0 down", NULL, 0);
		memset(((linux_sta_private_data_t *)(sta_inf_object.sta_private_data))->last_ssid, 0,
				SSID_MAX_LEN);
		memset(((linux_sta_private_data_t *)(sta_inf_object.sta_private_data))->last_bssid, 0,
				18);
		if(sta_inf_object.sta_event_handle != NULL){
			free(sta_inf_object.sta_event_handle);
			sta_inf_object.sta_event_handle = NULL;
		}
		sta_inf_object.sta_init_flag = WMG_FALSE;
		sta_inf_object.sta_auto_reconn = WMG_FALSE;
		sta_inf_object.sta_event_cb = NULL;
	} else {
		WMG_DEBUG("linux supplicant already deinit\n");
	}
	return WMG_STATUS_SUCCESS;
}

/* Establishes the control and monitor socket connections on the interface */
static wmg_status_t linux_sta_inf_enable()
{
	char ip_need_buf[10] = {0};
	linux_sta_private_data_t *private_data = NULL;
	private_data = (linux_sta_private_data_t *)sta_inf_object.sta_private_data;
	get_config("ip_need", ip_need_buf, "0");
	private_data->ip_need = (uint8_t)atoi(ip_need_buf);

	init_wpad_para_t wpa_supplicant_para = {
		.wifi_mode = WIFI_STATION,
		.dispatch_event = wpas_dispatch_event,
		.linux_mode_private_data = sta_inf_object.sta_private_data,
	};

	/* Clear the DHCP gate before init_wpad() starts the monitor thread,
	 * otherwise a CONNECTED event delivered during init could be dropped
	 * by a gate left over from the previous session. */
	dhcp_owner_session_begin(sta_inf);

	if(init_wpad(wpa_supplicant_para)) {
		WMG_ERROR("init wpa_supplicant failed\n");
		return WMG_STATUS_FAIL;
	} else {
		if(evt_socket_init(sta_inf_object.sta_event_handle)) {
			WMG_ERROR("failed to initialize linux sta event socket\n");
			deinit_wpad(WIFI_STATION);
			return WMG_STATUS_FAIL;
		}
		WMG_DEBUG("init wpa_supplicant success\n");
		sta_inf_object.sta_enable_flag = WMG_TRUE;

		/*
		 * Race condition fix: wpa_supplicant may have already connected
		 * and sent CTRL-EVENT-CONNECTED before our monitor socket was
		 * attached. If so, the event thread will never trigger udhcpc.
		 * Check current state and trigger DHCP manually if already connected.
		 */
		if (linux_sta_cmd_state() == 0) {
			WMG_INFO("wpa_supplicant already connected before monitor attach, trigger DHCP manually\n");
			try_start_udhcpc(sta_inf);
		}

		return WMG_STATUS_SUCCESS;
	}
}

static wmg_status_t linux_sta_inf_disable()
{
	WMG_INFO("linux supplicant stop now\n");
	linux_sta_private_data_t *private_data = NULL;
	private_data = (linux_sta_private_data_t *)sta_inf_object.sta_private_data;
	private_data->ip_need = 0;
	/* Close the DHCP owner session FIRST: this blocks any late CONNECTED
	 * event from starting a client and invalidates an in-flight start,
	 * before the monitor/event producer is torn down below (WR-015). */
	dhcp_owner_session_end(sta_inf);
	/* Stop the per-interface DHCP client before tearing down the supplicant,
	 * so a STA close or a STA->AP mode switch on the same interface leaves no
	 * orphaned client racing on IP/route/DNS. */
	stop_udhcpc(sta_inf, 0);
	if (sta_inf_object.sta_enable_flag == WMG_TRUE) {
		if (deinit_wpad(WIFI_STATION) == WMG_STATUS_SUCCESS) {
			evt_socket_exit(sta_inf_object.sta_event_handle);
			sta_inf_object.sta_enable_flag = WMG_FALSE;
			return WMG_STATUS_SUCCESS;
		}
	} else {
		WMG_INFO("linux supplicant already has stop\n");
		return WMG_STATUS_SUCCESS;
	}
	return WMG_STATUS_FAIL;
}

#define scan_num 80
/* This function is used to obtain the encryption method by scanning
 * 1.You need to enter ssid or bssid to get the encryption method
 * 2.If you only input bssid but not ssid, this function will put the scanned ssid into ssid_buf,
 *   because wpa3 needs ssid. */
static wifi_secure_t scan_get_secure(int scan_time, char **ssid, char *ssid_buf, uint8_t *bssid)
{
	uint32_t get_num = 0;
	int i = 0, get_scan_match_results = 0;
	WMG_DEBUG("linux os scan get secure\n");
	wifi_scan_result_t scan_res[scan_num];
	memset(&scan_res, 0, sizeof(wifi_scan_result_t) * scan_num);
	get_scan_results_para_t get_scan_results;
	memset(&get_scan_results, 0, sizeof(get_scan_results_para_t));
	get_scan_results.scan_results = scan_res;
	get_scan_results.ssid = NULL;
	get_scan_results.bss_num = &get_num;
	get_scan_results.arr_size = scan_num;

	if(wpa_get_scan_results(&get_scan_results) == WMG_STATUS_SUCCESS) {
		/* bss_num counts every discovered BSS, not only the stored ones */
		for (; i < (int)get_num && i < scan_num; i++) {
			if(!memcmp(bssid, scan_res[i].bssid, 6)) {
				get_scan_match_results = 1;
				if(*ssid == NULL) {
					if(strlen(scan_res[i].ssid) > SSID_MAX_LEN) {
						WMG_ERROR("scan get ssid is too log(%d)\n", strlen(scan_res[i].ssid));
						return WIFI_SEC_UNKNOWN;
					}
					strcpy(ssid_buf, scan_res[i].ssid);
					*ssid = ssid_buf;
					WMG_DEBUG("Input ssid is null, use bssid to update ssid(%s)\n", *ssid);
				}
			} else if(*ssid != NULL) {
				if(!strcmp(scan_res[i].ssid, *ssid)) {
					get_scan_match_results = 1;
				}
			}
			if(get_scan_match_results) {
				if((scan_res[i].key_mgmt) & WIFI_SEC_WPA3_PSK) {
					return WIFI_SEC_WPA3_PSK;
				}
				if((scan_res[i].key_mgmt) & WIFI_SEC_WPA2_PSK_SHA256) {
					return WIFI_SEC_WPA2_PSK_SHA256;
				}
				if((scan_res[i].key_mgmt) & WIFI_SEC_WPA2_PSK) {
					return WIFI_SEC_WPA2_PSK;
				}
				if((scan_res[i].key_mgmt) & WIFI_SEC_WPA_PSK) {
					return WIFI_SEC_WPA_PSK;
				}
				if((scan_res[i].key_mgmt) & WIFI_SEC_WEP) {
					return WIFI_SEC_WEP;
				}
				if(scan_res[i].key_mgmt == WIFI_SEC_NONE) {
					return WIFI_SEC_NONE;
				}
				break;
			}
		}
	}

	WMG_ERROR("linux os scan get secure failed\n");
	return WIFI_SEC_UNKNOWN;
}

static wmg_status_t linux_supplicant_connect_to_ap(wifi_sta_cn_para_t *cn_para)
{
	wmg_status_t ret;
	int try_cnt = 0, poll_timeout_try_cnt = 0, ret_tmp, netid = -1, i = 0;
	int event = -1;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	char ssid_buf[SSID_MAX_LEN + 1] = {0};
	uint8_t wep_auth_flag = 0, network_comp_flag = 0, bssid[6] = {0};
	// linux_sta_private_data_t *private_data = NULL;
	// private_data = (linux_sta_private_data_t *)sta_inf_object.sta_private_data;

	if((cn_para->ssid == NULL) && (!memcmp(cn_para->bssid, bssid, 6))) {
		WMG_ERROR("linux os ssid and bssid are both NULL\n");
		wpas_event_notify_to_sta_dev(WIFI_DISCONNECTED);
		return WMG_STATUS_UNSUPPORTED;
	}

	if(cn_para->sec == WIFI_SEC_UNKNOWN) {
		if((cn_para->sec = scan_get_secure(2, (char **)&(cn_para->ssid), ssid_buf, cn_para->bssid)) == WIFI_SEC_UNKNOWN) {
			WMG_ERROR("linux os can't scan get secure\n");
			wpas_event_notify_to_sta_dev(WIFI_DISCONNECTED);
			return WMG_STATUS_UNSUPPORTED;
		} else {
			WMG_DEBUG("linux os get secure %d\n", cn_para->sec);
		}
	}

	if(cn_para->sec & WIFI_SEC_EAP) {
		WMG_ERROR("linux os unsupport eap encrypt connect\n");
		wpas_event_notify_to_sta_dev(WIFI_DISCONNECTED);
		return WMG_STATUS_UNSUPPORTED;
	}

	network_comp_flag = wpa_network_comp(cn_para->ssid, cn_para->password, cn_para->sec, &netid);
	if((network_comp_flag & NETWORK_BP_EXIS) && (network_comp_flag & NETWORK_BP_CONET)) {
		/* waiting for connection (wpa/wpa2/wpa3)*/
		if(!linux_sta_cmd_status(reply)) {
			do
			{
				if(!wpa_parse_status(reply, NULL)) {
					WMG_INFO("wifi is already connected to %s\n", cn_para->ssid);
					wpas_event_notify_to_sta_dev(WIFI_CONNECTED);
					return WMG_STATUS_SUCCESS;
				}
				sleep(1);
				i++;
			}while(i < 3);
		}
		WMG_WARNG("network %d may not be connected, delete it and reconnect\n");
		sprintf(cmd, "REMOVE_NETWORK %d", netid);
		if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
			WMG_ERROR("remove network %d fail!\n", netid);
		}
		netid = -1;
	}

wep_auth_try:
	ret_tmp = evt_socket_clear(sta_inf_object.sta_event_handle);
	if (ret_tmp)
		WMG_WARNG("failed to clear event socket\n");

	ret_tmp = wpas_select_network(&netid, cn_para, wep_auth_flag);
	if (ret_tmp) {
		WMG_ERROR("failed to config network\n");
		wpas_event_notify_to_sta_dev(WIFI_DISCONNECTED);
		return WMG_STATUS_FAIL;
	}

	ret = WMG_STATUS_FAIL;
	/* wait connect event */
	while ((try_cnt < EVENT_TRY_MAX) && (poll_timeout_try_cnt < EVENT_POLL_TIMEOUT_TRY_MAX)) {
		ret_tmp = evt_read(sta_inf_object.sta_event_handle, &event);
		if (ret_tmp > 0) {
			WMG_DEBUG("receive wpas event '%s'\n", wmg_sta_event_to_str(event));
			if (event == WIFI_CONNECTED) {
				ret = WMG_STATUS_NOT_READY;
			} else if (event == WIFI_DHCP_START) {
				ret = WMG_STATUS_NOT_READY;
			} else if (event == WIFI_DHCP_TIMEOUT) {
				WMG_WARNG("Receive wpas event dhcp time out\n");
				ret = WMG_STATUS_FAIL;
				break;
			} else if (event == WIFI_DHCP_SUCCESS) {
				ret = WMG_STATUS_SUCCESS;
				break;
			} else if (event == WIFI_PASSWORD_INCORRECT) {
				WMG_WARNG("Receive wpas event password incorrect\n");
				break;
			} else if (event == WIFI_NETWORK_NOT_FOUND) {
			} else if (event == WIFI_ASSOC_REJECT) {
				WMG_WARNG("Receive wpas event assoc reject\n");
				break;
			} else {
				/* other event */
				try_cnt++;
			}
		} else {
			poll_timeout_try_cnt++;
		}
	}

	/* wep we need try auth_alg = shared*/
	if((ret != WMG_STATUS_SUCCESS) && (cn_para->sec == WIFI_SEC_WEP) && (wep_auth_flag == 0)) {
		WMG_WARNG("wep connect use auth_alg open fail, now try auth_alg shared\n");
		wep_auth_flag = 1;
		goto wep_auth_try;
	}

	if (ret == WMG_STATUS_SUCCESS) {
		/* Update to file wpa_supplicant.conf based on whether auto reconnect is performed */
		sta_inf_object.sta_auto_reconn = WMG_TRUE;
		wpa_conf_enable_or_disable_all_networks(sta_inf_object.sta_auto_reconn);
	}

	if (ret != WMG_STATUS_SUCCESS) {
		sprintf(cmd, "%s %d", "REMOVE_NETWORK", netid);
		cmd[CMD_MAX_LEN - 1] = '\0';
		ret_tmp = linux_command_to_supplicant(cmd, reply, sizeof(reply));
		if (!ret_tmp)
			WMG_DEBUG("remove network %d because of connection failure\n", netid);
		else
			WMG_WARNG("failed to remove network %d\n", netid);
	}

	return ret;
}

static wmg_status_t linux_supplicant_auto_connect(char *ssid)
{
	wmg_status_t ret;
	int netid;
	int try_cnt = 0, ret_tmp;
	int event = -1;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	// linux_sta_private_data_t *private_data = NULL;
	// private_data = (linux_sta_private_data_t *)sta_inf_object.sta_private_data;

	if(ssid == NULL) {
		WMG_ERROR("linux: ssid is null\n");
		return WMG_STATUS_INVALID;
	}
	ret_tmp = wpa_conf_ssid2netid(ssid, &netid);
	if(ret_tmp == -1) {
		WMG_WARNG("%s is not in wpa_supplicant.conf!\n", ssid);
		return WMG_STATUS_UNSUPPORTED;
	} else if(ret_tmp == 1) {
		WMG_INFO("%s already connected\n", ssid);
		return WMG_STATUS_SUCCESS;
	}

	ret_tmp = evt_socket_clear(sta_inf_object.sta_event_handle);
	if (ret_tmp)
		WMG_WARNG("failed to clear event socket\n");

	sprintf(cmd, "SELECT_NETWORK %d", netid);
	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("failed to select network %d, reply %s\n", netid, reply);
	}

	ret = WMG_STATUS_FAIL;
	/* wait connect event */
	while (try_cnt < EVENT_TRY_MAX) {
		ret_tmp = evt_read(sta_inf_object.sta_event_handle, &event);
		if (ret_tmp > 0) {
			WMG_DUMP("receive wpas event '%s'\n", wmg_sta_event_to_str(event));
			if (event == WIFI_CONNECTED) {
				ret = WMG_STATUS_NOT_READY;
			} else if (event == WIFI_DHCP_START) {
				ret = WMG_STATUS_NOT_READY;
			} else if (event == WIFI_DHCP_TIMEOUT) {
				ret = WMG_STATUS_FAIL;
				break;
			} else if (event == WIFI_DHCP_SUCCESS) {
				ret = WMG_STATUS_SUCCESS;
				break;
			} else if (event == WIFI_PASSWORD_INCORRECT) {
				break;
			} else if (event == WIFI_NETWORK_NOT_FOUND) {
			} else if (event == WIFI_ASSOC_REJECT) {
				break;
			} else {
				/* other event */
				try_cnt++;
			}
		} else {
			try_cnt++;
		}
	}

	if (ret != WMG_STATUS_SUCCESS) {
		if(try_cnt >= EVENT_TRY_MAX) {
			WMG_ERROR("--event try more than %d times--\n", EVENT_TRY_MAX);
		}
		sprintf(cmd, "%s %d", "REMOVE_NETWORK", netid);
		cmd[CMD_MAX_LEN - 1] = '\0';
		ret_tmp = linux_command_to_supplicant(cmd, reply, sizeof(reply));
		if (!ret_tmp)
			WMG_INFO("remove network %s because of connection failure\n", ssid);
		else
			WMG_WARNG("failed to remove network %d\n", netid);
	}

	return ret;
}

static wmg_status_t linux_supplicant_disconnect_to_ap(void)
{
	wmg_status_t ret;
	sta_inf_object.sta_auto_reconn = WMG_FALSE;
	if(linux_sta_cmd_state() != 0){
		WMG_DEBUG("sta is disconnected, need not to disconnect to ap\n");
		return WMG_STATUS_SUCCESS;
	}
	ret = linux_sta_cmd_disconnect();
	if (ret) {
		WMG_ERROR("failed to disconnect ap\n");
		return ret;
	}
	/* stop_udhcpc handles kill of DHCP client + flush of IPv4/IPv6;
	 * we call it explicitly here so event loss cannot leak the client. */
	stop_udhcpc(sta_inf, 0);
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_supplicant_get_info(wifi_sta_info_t *sta_info)
{
	wmg_status_t ret;
	int i;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	wifi_wpa_status_t wifi_wpa_status;
	char tmp_char[128] = {0};
	char *pch = NULL;

	ret = linux_sta_cmd_status(reply);
	if(ret != WMG_STATUS_SUCCESS) {
		return ret;
	}

	memset(&wifi_wpa_status, 0, sizeof(wifi_wpa_status));

	if(wpa_parse_status(reply, &wifi_wpa_status)) {
		WMG_ERROR("failed to parse station info\n");
		return WMG_STATUS_FAIL;
	}

	sta_info->id = atoi(wifi_wpa_status.id);
	sta_info->freq = atoi(wifi_wpa_status.freq);

	memset(tmp_char, 0, 128);
	strcpy(tmp_char, wifi_wpa_status.bssid);
	pch = strtok(tmp_char, ":");
	for(i = 0;(pch != NULL) && (i < 6); i++){
		sta_info->bssid[i] = char2uint8(pch);
		pch = strtok(NULL, ":");
	}

	strcpy(sta_info->ssid, wifi_wpa_status.ssid);

	memset(tmp_char, 0, 128);
	strcpy(tmp_char, wifi_wpa_status.address);
	pch = strtok(tmp_char, ":");
	for(i = 0; (pch != NULL) && (i < 6); i++){
		sta_info->mac_addr[i] = char2uint8(pch);
		pch = strtok(NULL, ":");
	}

	memset(tmp_char, 0, 128);
	strcpy(tmp_char, wifi_wpa_status.ip_address);
	pch = strtok(tmp_char, ".");
	for(i = 0; (pch != NULL) && (i < 4); i++){
		sta_info->ip_addr[i] = atoi(pch);
		pch = strtok(NULL, ".");
	}

	if (strncmp(wifi_wpa_status.key_mgmt, "SAE", 3) == 0) {
		sta_info->sec |= WIFI_SEC_WPA3_PSK;
	}
	if (strncmp(wifi_wpa_status.key_mgmt, "WPA2-PSK-SHA256", 15) == 0) {
		sta_info->sec |= WIFI_SEC_WPA2_PSK_SHA256;
	} else if (strncmp(wifi_wpa_status.key_mgmt, "WPA2-PSK", 8) == 0) {
		sta_info->sec |= WIFI_SEC_WPA2_PSK;
	}
	if (strncmp(wifi_wpa_status.key_mgmt, "WPA-PSK", 7) == 0) {
		sta_info->sec |= WIFI_SEC_WPA_PSK;
	}
	if (strncmp(wifi_wpa_status.key_mgmt, "NONE", 4) == 0) {
		if (strncmp(wifi_wpa_status.pairwise_cipher, "NONE", 4) == 0) {
			sta_info->sec |= WIFI_SEC_NONE;
		} else {
			sta_info->sec |= WIFI_SEC_WEP;
		}
	}

	sprintf(cmd, "%s", "SIGNAL_POLL");
	ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
	if (ret) {
		WMG_ERROR("failed to get signal of wifi station, reply %s\n", reply);
		return WMG_STATUS_FAIL;
	}
	ret = wpa_parse_signal_info(reply, sta_info);
	if(ret) {
		WMG_ERROR("failed to parse signal info\n");
		return WMG_STATUS_FAIL;
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_supplicant_list_networks(wifi_sta_list_t *sta_list)
{
	wmg_status_t ret;
	int i = 0, list_entry_num = 0;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[REPLY_BUF_SIZE] = {0};
	/* Keep one extra slot as the NULL sentinel used by the loops below */
	char *pch_entry_p[LIST_ENTRY_NUME_MAX + 1] = {0};
	char utf8_tmp_buf[SSID_MAX_LEN * 4] = {0};
	char *pch_entry = NULL;
	char *pch_item = NULL;
	char *delim_entry = "'\n'";
	char *delim_item = "'\t'";

	if (sta_list == NULL) {
		WMG_ERROR("invalid parameters\n");
		return WMG_STATUS_INVALID;
	}

	if(sta_list->list_num > LIST_ENTRY_NUME_MAX) {
		WMG_WARNG("Sys only support list %d entry\n", LIST_ENTRY_NUME_MAX);
		list_entry_num = LIST_ENTRY_NUME_MAX;
	} else {
		list_entry_num = sta_list->list_num;
	}

	sprintf(cmd, "%s", "LIST_NETWORKS");
	ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
	if (ret) {
		WMG_ERROR("failed to list networks, reply %s\n", reply);
		return ret;
	}

	if(strlen(reply) < 34){
		WMG_INFO("Here has no entry save\n");
		sta_list->sys_list_num = 0;
		return WMG_STATUS_SUCCESS;
	}

	pch_entry = strtok((reply + 34), delim_entry);
	while((pch_entry != NULL) && (i < list_entry_num)) {
		pch_entry_p[i] = pch_entry;
		i++;
		pch_entry = strtok(NULL, delim_entry);
	}

	sta_list->sys_list_num = i;
	WMG_DEBUG("sys save num is:%d, list buff num is:%d\n", i, sta_list->list_num);

	for(i = 0; pch_entry_p[i] != NULL; i++) {
		pch_item = strtok(pch_entry_p[i], delim_item);
		if(pch_item != NULL) {
			sta_list->list_nod[i].id = atoi(pch_item);
		}
		pch_item = strtok(NULL, delim_item);
		memset(utf8_tmp_buf, '\0', SSID_MAX_LEN * 4);
		if((pch_item != NULL) && (strlen(pch_item) <= (SSID_MAX_LEN * 4)) &&
				(convert_utf8(pch_item, utf8_tmp_buf) <= SSID_MAX_LEN)){
			strcpy((sta_list->list_nod[i].ssid), utf8_tmp_buf);
		} else {
			strcpy((sta_list->list_nod[i].ssid), "NULL");
			WMG_WARNG("An illegal ssid was obtained and has been set to NULL\n");
		}
		pch_item = strtok(NULL, delim_item);
		if(pch_item != NULL) {
			memcpy((sta_list->list_nod[i].bssid), pch_item, sizeof(sta_list->list_nod[i].bssid));
		}
		pch_item = strtok(NULL, delim_item);
		if(pch_item != NULL) {
			strcpy((sta_list->list_nod[i].flags), pch_item);
		} else {
			strcpy((sta_list->list_nod[i].flags), "NULL");
		}
	}

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_supplicant_remove_networks(char *ssid)
{
	wmg_status_t ret = WMG_STATUS_FAIL;
	int i = 0;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[REPLY_BUF_SIZE] = {0};
	int net_id;
	/* Keep one extra slot as the NULL sentinel used by the loops below */
	char *pch_entry_p[LIST_ENTRY_NUME_MAX + 1] = {0};
	char *pch_entry = NULL;
	char *pch_item = NULL;
	char *delim_entry = "'\n'";
	char *delim_item = "'\t'";

	if(ssid != NULL) {
		WMG_DEBUG("remove network(%s) ...\n", ssid);
		/* check AP is exist in wpa_supplicant.conf */
		ret = wpa_conf_ssid2netid(ssid, &net_id);
		if (ret == -1) {
			WMG_WARNG("%s is not in wpa_supplicant.conf!\n", ssid);
			return WMG_STATUS_INVALID;
		}

		/* cancel saved in wpa_supplicant.conf */
		cmd[CMD_MAX_LEN - 1] = '\0';
		sprintf(cmd, "REMOVE_NETWORK %d", net_id);
		ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
		if (ret) {
			WMG_ERROR("do remove network %s error!\n", net_id);
			return WMG_STATUS_FAIL;
		}

	} else {
		WMG_DEBUG("remove all network ...\n", ssid);
		sprintf(cmd, "%s", "LIST_NETWORKS");
		cmd[CMD_MAX_LEN - 1] = '\0';
		ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
		if (ret) {
			WMG_ERROR("failed to list networks, reply %s\n", reply);
			return ret;
		}

		if(strlen(reply) < 34){
			WMG_INFO("Here has no entry save\n");
			return WMG_STATUS_INVALID;
		}

		pch_entry = strtok((reply + 34), delim_entry);
		while(pch_entry != NULL && i < LIST_ENTRY_NUME_MAX) {
			pch_entry_p[i] = pch_entry;
			i++;
			pch_entry = strtok(NULL, delim_entry);
		}

		WMG_DEBUG("sys save num is:%d\n", i);

		for(i = 0; pch_entry_p[i] != NULL; i++) {
			pch_item = strtok(pch_entry_p[i], delim_item);
			cmd[CMD_MAX_LEN - 1] = '\0';
			sprintf(cmd, "REMOVE_NETWORK %s", pch_item);
			ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
			if (ret) {
				WMG_ERROR("do remove network %s error!\n", pch_item);
				return WMG_STATUS_FAIL;
			}
		}
	}

	/* save config */
	sprintf(cmd, "%s", "SAVE_CONFIG");
	ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
	if (ret) {
		WMG_ERROR("do save config error!\n");
		return WMG_STATUS_FAIL;
	}

	return ret;
}

static wmg_status_t linux_platform_extension(int cmd, void* cmd_para,int *erro_code)
{
	switch (cmd) {
		case STA_CMD_CONNECT:
			return linux_supplicant_connect_to_ap((wifi_sta_cn_para_t *)cmd_para);
		case STA_CMD_DISCONNECT:
			return linux_supplicant_disconnect_to_ap();
		case STA_CMD_GET_INFO:
			return linux_supplicant_get_info((wifi_sta_info_t *)cmd_para);
		case STA_CMD_LIST_NETWORKS:
			return linux_supplicant_list_networks((wifi_sta_list_t *)cmd_para);
		case STA_CMD_REMOVE_NETWORKS:
			return linux_supplicant_remove_networks((char *)cmd_para);
		case STA_CMD_SET_AUTO_RECONN:
			return wpa_set_auto_reconn(*((wmg_bool_t *)cmd_para));
		case STA_CMD_SET_AUTO_CONN:
			return linux_supplicant_auto_connect((char *)cmd_para);
		case STA_CMD_GET_SCAN_RESULTS:
			return wpa_get_scan_results((get_scan_results_para_t *)cmd_para);
		default:
			return WMG_FALSE;
	}
}

static linux_sta_private_data_t linux_sta_private_data = {
	.last_ssid = {0},
	.last_bssid = {0},
	.last_time = 0,
	.ip_need = 0,
};

static wmg_sta_inf_object_t sta_inf_object = {
	.sta_init_flag = WMG_FALSE,
	.sta_enable_flag = WMG_FALSE,
	.sta_auto_reconn = WMG_TRUE,
	.sta_event_cb = NULL,
	.sta_event_handle = NULL,
	.sta_private_data = &linux_sta_private_data,

	.sta_inf_init = linux_sta_inf_init,
	.sta_inf_deinit = linux_sta_inf_deinit,
	.sta_inf_enable = linux_sta_inf_enable,
	.sta_inf_disable = linux_sta_inf_disable,
	.sta_platform_extension = linux_platform_extension,
};

wmg_sta_inf_object_t* sta_linux_inf_object_register(void)
{
	return &sta_inf_object;
}
