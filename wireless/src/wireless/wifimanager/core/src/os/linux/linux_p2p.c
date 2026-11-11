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
#include <wmg_p2p.h>
#include <unistd.h>
#include <linux_p2p.h>
#include <linux_common.h>
#include <linux_get_config.h>

static wmg_p2p_inf_object_t p2p_inf_object;

#define P2P_ROLE_NON  0
#define P2P_ROLE_GO   1
#define P2P_ROLE_GC   2

#define P2P_GROUP_INFO_STR_MAX_LEN 256
#define P2P_INF_LEN                24
#define P2P_PSK_LEN                128
#define P2P_PASS_LEN               64
#define DEF_NET_SEGMENT            "192.168.6"

typedef struct {
	int role;
	char p2p_inf[P2P_INF_LEN];
	char p2p_ssid[SSID_MAX_LEN];
	int p2p_freq;
	int p2p_discover_time;
	int p2p_go_intent;
	/*use as gc*/
	char p2p_psk[P2P_PSK_LEN];
	/*use as go*/
	char p2p_passphrase[P2P_PASS_LEN];
	uint8_t go_dev_addr[6];
	char net_segment[20];
} p2p_dev_private_t;

typedef struct {
	char bssid[18];
	char freq[16];
	char ssid[SSID_MAX_LEN + 1];
	char id[8];
	char mode[8];
	char pairwise_cipher[64];
	char group_cipher[64];
	char key_mgmt[32];
	char ip_address[16];
	char p2p_device_address[16];
	char address[18];
} wifi_wpa_p2p_gp_status_t;

static int linux_command_to_supplicant(char const *cmd, char *reply, size_t reply_len)
{
	return command_to_wpad(cmd, reply, reply_len, WIFI_P2P, NULL);
}

static void replace_ssid_name_spaces(char *info_str)
{
	char new_buf[P2P_GROUP_INFO_STR_MAX_LEN]={0};
	int state = 0;
	int i = 0;
	char *old_buf = info_str;

	do {
		if (*info_str == '"') {
			state++;  /* get '"' */
			new_buf[i++] = *info_str;
		} else if (state == 1 && *info_str == ' ') {
			new_buf[i++] = '\t'; /* Use \t instead of Spaces for temporary */
		} else {
			new_buf[i++] = *info_str;
		}
	} while (*info_str++ != '\0');
	new_buf[i] = '\0';
	WMG_DEBUG("new p2p_group_info_str:%s\n", new_buf);
	strcpy(old_buf, new_buf);
}

static wmg_status_t parse_group_started_info(char *p2p_group_info_str)
{
	char *pch = NULL;
	char *delim = " ";
	p2p_dev_private_t *private_data  = (p2p_dev_private_t *)p2p_inf_object.p2p_private_data;
	if(!p2p_group_info_str) {
		WMG_ERROR("p2p group started info str is null\n");
		return WMG_STATUS_FAIL;
	}
	replace_ssid_name_spaces(p2p_group_info_str);
	memset(private_data->p2p_inf, 0, P2P_INF_LEN);
	memset(private_data->p2p_ssid, 0, SSID_MAX_LEN);
	memset(private_data->p2p_psk, 0, P2P_PSK_LEN);
	memset(private_data->p2p_passphrase,0, P2P_PASS_LEN);

	/* Split according to p2p group started format */
	// get p2p group started info: p2p inf
	pch = strtok(p2p_group_info_str, delim);
	if (pch == NULL) {
		goto erro;
	}
	strncpy(private_data->p2p_inf, pch, strlen(pch));
	private_data->p2p_inf[P2P_INF_LEN - 1] = 0;
	// get p2p group started info: p2p role
	pch = strtok(NULL, delim);
	if (pch == NULL) {
		goto erro;
	}
	if(!strncmp(pch,"GO", 2)){
		private_data->role = P2P_ROLE_GO;
	} else if(!strncmp(pch,"client", 6)){
		private_data->role = P2P_ROLE_GC;
	} else {
		goto erro;
	}
	// get p2p group started info: p2p role
	pch = strtok(NULL, delim);
	if (pch == NULL) {
		goto erro;
	}
	if(!strncmp(pch,"ssid=\"", 6)){
		pch += 6;
		char *old_pch = pch;
		while (*pch++ != '\0') {
			if (*pch == '\t')
				*pch = ' '; /* replace \t with space */
		}
		strncpy(private_data->p2p_ssid, old_pch, strlen(old_pch));
		private_data->p2p_ssid[(strlen(old_pch) - 1)] = 0;
	} else{
		goto erro;
	}
	// get p2p group started info: p2p freq
	pch = strtok(NULL, delim);
	if (pch == NULL) {
		goto erro;
	}
	if(!strncmp(pch,"freq=", 5)){
		pch += 5;
		private_data->p2p_freq = atoi(pch);
	} else{
		goto erro;
	}
	// get p2p group started info: p2p passphrase(go) or p2p psk(gc);
	pch = strtok(NULL, delim);
	if (pch == NULL) {
		goto erro;
	}
	if(!strncmp(pch,"passphrase=\"", 12)){
		pch += 12;
		if(strlen(pch) < P2P_PASS_LEN) {
			strncpy(private_data->p2p_passphrase, pch, strlen(pch));
			private_data->p2p_passphrase[strlen(pch)] = 0;
		} else {
			WMG_ERROR("p2p passphrase is longer than %d\n", P2P_PASS_LEN);
			goto erro;
		}
	} else if(!strncmp(pch,"psk=", 4)){
		pch += 4;
		if(strlen(pch) < P2P_PSK_LEN) {
			strncpy(private_data->p2p_psk, pch, strlen(pch));
			private_data->p2p_psk[strlen(pch)] = 0;
		} else {
			WMG_ERROR("p2p psk is longer than %d\n", P2P_PSK_LEN);
			goto erro;
		}
	} else {
		goto erro;
	}
	// get p2p group started info: p2p go dev addr;
	pch = strtok(NULL, delim);
	if (pch == NULL) {
		goto erro;
	}
	if(!strncmp(pch,"go_dev_addr=", 12)){
		pch += 12;
		char p2p_addr[18];
		char *p = p2p_addr;
		strncpy(p2p_addr, pch, 17);
		p2p_addr[17] = 0;
		int i;
		for(i = 0;(i < 6); i++){
			private_data->go_dev_addr[i] = char2uint8(p);
			p += 3;
		}
	} else{
		goto erro;
	}

	WMG_DEBUG("parse_group_started_info:\nrole:%d\np2p_inf:%s\np2p_ssid:%s\np2p_freq:%d\np2p_psk:%s\np2p_passphrase:%s\ngo_dev_addr: %x%x:%x%x:%x%x:%x%x:%x%x:%x%x\n",
				private_data->role,
				private_data->p2p_inf,
				private_data->p2p_ssid,
				private_data->p2p_freq,
				private_data->p2p_psk,
				private_data->p2p_passphrase,
				(private_data->go_dev_addr[0] >> 4),
				(private_data->go_dev_addr[0] & 0xf),
				(private_data->go_dev_addr[1] >> 4),
				(private_data->go_dev_addr[1] & 0xf),
				(private_data->go_dev_addr[2] >> 4),
				(private_data->go_dev_addr[2] & 0xf),
				(private_data->go_dev_addr[3] >> 4),
				(private_data->go_dev_addr[3] & 0xf),
				(private_data->go_dev_addr[4] >> 4),
				(private_data->go_dev_addr[4] & 0xf),
				(private_data->go_dev_addr[5] >> 4),
				(private_data->go_dev_addr[5] & 0xf));

	p2p_inf_object.p2p_role = private_data->role;
	return WMG_STATUS_SUCCESS;

erro:
	private_data->role = P2P_ROLE_NON;
	return WMG_STATUS_FAIL;
}

static void wpas_event_notify_to_p2p_dev(wifi_p2p_event_t event)
{
	if (p2p_inf_object.p2p_event_cb) {
		p2p_inf_object.p2p_event_cb(event);
	}
}

static void wpas_event_notify(wifi_p2p_event_t event)
{
	evt_send(p2p_inf_object.p2p_event_handle, event);
	wpas_event_notify_to_p2p_dev(event);
}

static void p2p_try_start_udhcpc(char *inf)
{
	WMG_DEBUG("**************************wait 1 seconds********************************\n");
	sleep(1);
	WMG_DEBUG("*************************p2p try start udhcpc*******************************\n");

	if(!is_ip_exist(inf, IPV4_ADDR_BM)) {
		WMG_DEBUG("%s IPv4 is not exist, need to start_udhcpc\n", inf);
		start_udhcpc(inf, IPV4_ADDR_BM);
		if(is_ip_exist(inf, IPV4_ADDR_BM)) {
			wpas_event_notify(WIFI_P2P_GROUP_DHCP_SUCCESS);
		} else {
			wpas_event_notify(WIFI_P2P_GROUP_DHCP_FAILURE);
		}
	}
	WMG_DEBUG("**************************try udhcpc end********************************\n");
}

static int wpas_dispatch_event(const char *event_str, int nread)
{
	int ret, i = 0, event = 0;
	char event_nae[CMD_MAX_LEN];
	char cmd[CMD_MAX_LEN] = {0}, reply[EVENT_BUF_SIZE] = {0};
	char *nae_start = NULL, *nae_end = NULL;
	char *event_data = NULL;
	char p2p_maddr[18] = {0};
	p2p_dev_private_t *p2p_dev_private = (p2p_dev_private_t *)p2p_inf_object.p2p_private_data;

	if (!event_str || !event_str[0]) {
		WMG_WARNG("wpa_supplicant event is NULL!\n");
		return 0;
	}

	WMG_INFO("receive wpa p2p event: %s\n", event_str);
	if (strstr(event_str, "P2P-DEVICE-FOUND")) {
		wpas_event_notify(WIFI_P2P_DEV_FOUND);
	} else if (strstr(event_str, "P2P-DEVICE-LOST")) {
		wpas_event_notify(WIFI_P2P_DEV_LOST);
	/* Passively received connection request */
	} else if (strstr(event_str, "P2P-PROV-DISC-PBC-REQ")) {
		wpas_event_notify(WIFI_P2P_PBC_REQ);
		if(p2p_inf_object.auto_connect == WMG_TRUE) {
			event_data = strchr(event_str, ' ');
			if (event_data) {
				event_data++;
				strncpy(p2p_maddr, event_data, 17);
				sprintf(cmd, "P2P_CONNECT %s pbc", p2p_maddr);
				cmd[CMD_MAX_LEN - 1] = '\0';
				if (linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
					WMG_ERROR("wpa_supplicant p2p auto_connect %s faile, reply %s\n",
							((auto_connect_t *)(p2p_inf_object.p2p_private_data))->p2p_mac_addr, reply);
				}
			}
		}
	} else if (strstr(event_str, "P2P-GO-NEG-REQUEST")) {
		wpas_event_notify(WIFI_P2P_GO_NEG_RQ);
		if(p2p_inf_object.auto_connect == WMG_TRUE) {
			event_data = strchr(event_str, ' ');
			if (event_data) {
				event_data++;
				strncpy(p2p_maddr, event_data, 17);
				if(p2p_dev_private->p2p_go_intent != -1) {
					sprintf(cmd, "P2P_CONNECT %s pbc go_intent=%d", p2p_maddr, p2p_dev_private->p2p_go_intent);
				} else {
					sprintf(cmd, "P2P_CONNECT %s pbc", p2p_maddr);
				}
				cmd[CMD_MAX_LEN - 1] = '\0';
				if (linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
					WMG_ERROR("wpa_supplicant p2p auto_connect %s faile, reply %s\n",
							((auto_connect_t *)(p2p_inf_object.p2p_private_data))->p2p_mac_addr, reply);
				}
			}
		}
	} else if (strstr(event_str, "P2P-GO-NEG-SUCCESS")) {
		wpas_event_notify(WIFI_P2P_GO_NEG_SUCCESS);
	} else if (strstr(event_str, "P2P-GO-NEG-FAILURE")) {
		wpas_event_notify(WIFI_P2P_GO_NEG_FAILURE);
		p2p_inf_object.p2p_role = P2P_ROLE_NON;
	} else if (strstr(event_str, "P2P-GROUP-FORMATION-SUCCESS")) {
		wpas_event_notify(WIFI_P2P_GROUP_FOR_SUCCESS);
	} else if (strstr(event_str, "P2P-GROUP-FORMATION-FAILURE")) {
		wpas_event_notify(WIFI_P2P_GROUP_FOR_FAILURE);
		p2p_inf_object.p2p_role = P2P_ROLE_NON;
	} else if (strstr(event_str, "P2P-GROUP-STARTED")) {
		wpas_event_notify(WIFI_P2P_GROUP_STARTED);
		event_data = strchr(event_str, ' ');
		if (event_data)
			event_data++;
		if(parse_group_started_info(event_data) != WMG_STATUS_SUCCESS) {
			wpas_event_notify(WIFI_P2P_GROUP_DHCP_DNS_FAILURE);
			return WMG_STATUS_FAIL;
		} else if(p2p_inf_object.p2p_role == P2P_ROLE_GO) {
			/* go mode set ip address */
			memset(cmd,0,sizeof(cmd));
			sprintf(cmd, "ifconfig %s %s.1", ((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->p2p_inf,
				((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->net_segment);
			wmg_system(cmd, NULL, 0);

			/* go mode deletes the old dns configuration */
			//memset(cmd,0,sizeof(cmd));
			//sprintf(cmd, "sed -i \'/.*interface=p2p*/,+1d\' /etc/dnsmasq.conf");
			//wmg_system(cmd, NULL, 0);

			/* go mode set dns configuration */
			//memset(cmd,0,sizeof(cmd));
			//sprintf(cmd, "echo -e \"\ninterface=%s\ndhcp-range=%s.2,%s.255\" >> /etc/dnsmasq.conf",
			//		((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->p2p_inf,
			//		((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->net_segment,
			//		((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->net_segment);

			//sprintf(cmd, "sed -i \'/.*interface=p2p-wlan*/c\\interface=%s\' /etc/dnsmasq.conf",
			//	((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->p2p_inf);
			//wmg_system(cmd, NULL, 0);

			//wmg_system("/etc/init.d/dnsmasq restart", NULL, 0);
			if (start_linux_process("dnsmasq", "dnsmasq -C /etc/dnsmasq.conf -l /var/dnsmasq.leases -x /var/dnsmasq.pid")) {
				WMG_ERROR("start dnsmasq fail\n");
			} else {
				wpas_event_notify(WIFI_P2P_GROUP_DNS_SUCCESS);
			}
		} else if(p2p_inf_object.p2p_role == P2P_ROLE_GC) {
			p2p_try_start_udhcpc(((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->p2p_inf);
		}
	} else if (strstr(event_str, "P2P-GROUP-REMOVED")) {
		p2p_inf_object.p2p_role = P2P_ROLE_NON;
		wpas_event_notify(WIFI_P2P_GROUP_REMOVED);
	} else if (strstr(event_str, "P2P-CROSS-CONNECT-ENABLE")) {
		wpas_event_notify(WIFI_P2P_CROSS_CONNECT_ENABLE);
	} else if (strstr(event_str, "P2P-CROSS-CONNECT-DISABLE")) {
		p2p_inf_object.p2p_role = P2P_ROLE_NON;
		wpas_event_notify(WIFI_P2P_CROSS_CONNECT_DISABLE);
	} else if (strstr(event_str, "AP-STA-DISCONNECTED")) {
		sprintf(cmd, "P2P_GROUP_REMOVE %s",p2p_dev_private->p2p_inf);
		cmd[CMD_MAX_LEN - 1] = '\0';
		if (linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
			WMG_ERROR("wpa_supplicant p2p group remove %s failed, reply %s\n", p2p_dev_private->p2p_inf, reply);
		}
		wpas_event_notify(WIFI_P2P_AP_STA_DISCONNECTED);
		p2p_inf_object.p2p_role = P2P_ROLE_NON;
		WMG_INFO("----p2p: ap-sta disconnected----\n");
	} else if (strstr(event_str, "CTRL-EVENT-SCAN-STARTED")) {
		wpas_event_notify(WIFI_P2P_SCAN_STARTED);
	} else if (strstr(event_str, "CTRL-EVENT-SCAN-RESULTS")) {
		//wpas_event_notify(WIFI_P2P_SCAN_RESULTS);
	} else if (strstr(event_str, "P2P-FIND-STOPPED")) {
		wpas_event_notify(WIFI_P2P_FIND_STOPPED);
	} else {
		wpas_event_notify(WIFI_P2P_UNKNOWN);
	}

	return 0;
}

static wmg_status_t linux_p2p_inf_init(p2p_event_cb_t p2p_event_cb, void *p)
{
	if(p2p_inf_object.p2p_init_flag == WMG_FALSE) {
		WMG_INFO("linux p2p supplicant init now\n");

		p2p_inf_object.p2p_event_handle = (event_handle_t *)malloc(sizeof(event_handle_t));
		if(p2p_inf_object.p2p_event_handle != NULL) {
			memset(p2p_inf_object.p2p_event_handle, 0, sizeof(event_handle_t));
			p2p_inf_object.p2p_event_handle->evt_socket[0] = -1;
			p2p_inf_object.p2p_event_handle->evt_socket[1] = -1;
			p2p_inf_object.p2p_event_handle->evt_socket_enable = WMG_FALSE;
		} else {
			WMG_ERROR("failed to allocate memory for linux p2p event_handle\n");
			return WMG_STATUS_FAIL;
		}

		if(p2p_event_cb != NULL){
			p2p_inf_object.p2p_event_cb = p2p_event_cb;
		}
		p2p_inf_object.p2p_init_flag = WMG_TRUE;
	} else {
		WMG_INFO("linux supplicant already init\n");
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_p2p_inf_deinit(void *p)
{
	if(p2p_inf_object.p2p_init_flag == WMG_TRUE) {
		if(p2p_inf_object.p2p_event_handle != NULL){
			free(p2p_inf_object.p2p_event_handle);
			p2p_inf_object.p2p_event_handle = NULL;
		}
		p2p_inf_object.p2p_event_cb = NULL;
		p2p_inf_object.p2p_init_flag = WMG_FALSE;
	} else {
		WMG_INFO("linux p2p already init\n");
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_p2p_inf_enable()
{
	init_wpad_para_t wpa_supplicant_para = {
		.wifi_mode = WIFI_P2P,
		.dispatch_event = wpas_dispatch_event,
		.linux_mode_private_data = p2p_inf_object.p2p_private_data,
	};

	if(init_wpad(wpa_supplicant_para)) {
		WMG_ERROR("init wpa_supplicant failed\n");
		return WMG_STATUS_FAIL;
	} else {
		if(evt_socket_init(p2p_inf_object.p2p_event_handle)) {
			WMG_ERROR("failed to initialize linux p2p event socket\n");
			deinit_wpad(WIFI_P2P);;
		}
		WMG_DEBUG("init wpa_supplicant success\n");
		p2p_inf_object.p2p_enable_flag = WMG_TRUE;
		memset(((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->net_segment, 0, 20);
		get_config("p2p_net_segment", ((p2p_dev_private_t *)(p2p_inf_object.p2p_private_data))->net_segment, DEF_NET_SEGMENT);
		return WMG_STATUS_SUCCESS;
	}
}

static wmg_status_t linux_p2p_inf_disable()
{
	WMG_INFO("linux supplicant stop now\n");
	if (p2p_inf_object.p2p_enable_flag == WMG_TRUE) {
		if (deinit_wpad(WIFI_P2P) == WMG_STATUS_SUCCESS) {
			evt_socket_exit(p2p_inf_object.p2p_event_handle);
			p2p_inf_object.p2p_enable_flag = WMG_FALSE;
			return WMG_STATUS_SUCCESS;
		}
	} else {
		WMG_INFO("linux supplicant already has stop\n");
		return WMG_STATUS_SUCCESS;
	}
	return WMG_STATUS_FAIL;
}

static wmg_status_t linux_p2p_enable(wifi_p2p_config_t *p2p_config)
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	int p2p_go_intent = -1;
	char discover_time[256] = {0};
	p2p_dev_private_t *p2p_dev_private = (p2p_dev_private_t *)p2p_inf_object.p2p_private_data;

	if(p2p_config != NULL) {
		p2p_inf_object.auto_connect = p2p_config->auto_connect;
		if(p2p_inf_object.auto_connect) {
			if(p2p_config->discover_time) {
				p2p_dev_private->p2p_discover_time = p2p_config->discover_time;
			} else {
				if((get_config("wpa_p2p_discover_time", discover_time, NULL))) {
					WMG_DEBUG("get config wpa_p2p_discover_time fail, use default\n");
				} else {
					p2p_dev_private->p2p_discover_time = atoi(discover_time);
				}
			}
			if((p2p_config->p2p_go_intent >= 0) && (p2p_config->p2p_go_intent <= 15)) {
				p2p_dev_private->p2p_go_intent = p2p_config->p2p_go_intent;
			}
		}
	}

	if(p2p_dev_private->p2p_go_intent != -1) {
		sprintf(cmd, "SET p2p_go_intent %d", p2p_dev_private->p2p_go_intent);
		cmd[CMD_MAX_LEN - 1] = '\0';
		if (linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
			WMG_ERROR("wpa_supplicant p2p set p2p_go_intent %d failed, reply %s\n",
					p2p_dev_private->p2p_go_intent, reply);
			return WMG_STATUS_FAIL;
		} else {
			WMG_INFO("wpa_supplicant p2p set p2p_go_intent %d success\n",
					p2p_dev_private->p2p_go_intent);
		}
	} else {
		WMG_DEBUG("wpa_supplicant p2p go intent used default\n");
	}

	if(p2p_config->dev_name != NULL) {
		sprintf(cmd, "SET device_name %s", p2p_config->dev_name);
		cmd[CMD_MAX_LEN - 1] = '\0';
		if (linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
			WMG_ERROR("wpa_supplicant p2p set device_name %s failed, reply %s\n", p2p_config->dev_name, reply);
			return WMG_STATUS_FAIL;
		} else {
			WMG_INFO("wpa_supplicant p2p set device_name %s success\n", p2p_config->dev_name);
			strncpy(p2p_dev_private->p2p_ssid, p2p_config->dev_name, strlen(p2p_config->dev_name));
		}
	}

	if((p2p_inf_object.auto_connect) && (p2p_dev_private->p2p_discover_time > 0)) {
		sprintf(cmd, "P2P_FIND %d", p2p_dev_private->p2p_discover_time);
		cmd[CMD_MAX_LEN - 1] = '\0';
		if (linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
			WMG_ERROR("wpa_supplicant p2p find(%d) failed, reply %s\n", p2p_dev_private->p2p_discover_time, reply);
			return WMG_STATUS_FAIL;
		} else {
			WMG_INFO("wpa_supplicant p2p finding, discover for %d seconds\n", p2p_dev_private->p2p_discover_time);
		}
	}

	sprintf(cmd, "sed -i \'/.*interface=p2p-wlan*/c\\interface=p2p-wlan0-0\' /etc/dnsmasq.conf");
	wmg_system(cmd, NULL, 0);
	if (start_linux_process("dnsmasq", "dnsmasq -C /etc/dnsmasq.conf -l /var/dnsmasq.leases -x /var/dnsmasq.pid")) {
		WMG_ERROR("start dnsmasq fail\n");
		return WMG_STATUS_FAIL;
	}

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_p2p_disable()
{
	if(stop_linux_process("dnsmasq")) {
		WMG_ERROR("stop dnsmasq fail\n");
		return WMG_STATUS_FAIL;
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t command_p2p_peers(char *cmd, char *addr, char *dev_name)
{
	char reply[EVENT_BUF_SIZE] = {0};
	char *pos;

	if(linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_DEBUG("wpa_supplicant p2p peer failed, reply %s\n", reply);
		return WMG_STATUS_UNHANDLED;
	} else if (memcmp(reply, "FAIL", 4) == 0) {
		return WMG_STATUS_UNHANDLED;
	}

	pos = reply;
	while (*pos != '\0' && *pos != '\n')
		pos++;
	*pos++ = '\0';
	strcpy(addr, reply);
	pos = strtok(pos, "'\n'");
	while (pos != NULL) {
		if (strncmp(pos, "device_name=", 12) == 0) {
			strcpy(dev_name, (pos + 12));
		}
		pos = strtok(NULL, "'\n'");
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_p2p_find(int find_second)
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};

	if(find_second < 0) {
		sprintf(cmd, "%s", "P2P_FIND");
	} else if(find_second == 0){
		sprintf(cmd, "%s", "P2P_STOP_FIND");
	} else {
		sprintf(cmd, "%s %d", "P2P_FIND", find_second);
	}
	cmd[CMD_MAX_LEN - 1] = '\0';
	if (linux_command_to_supplicant(cmd, reply, sizeof(reply))) {
		WMG_ERROR("wpa_supplicant p2p find cmd(%s) failed, reply %s\n", cmd, reply);
		return WMG_STATUS_FAIL;
	}

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_p2p_get_peers(find_results_para_t *results_para)
{
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	char *pch = NULL;
	char addr[32] = {0};
	char addr_save[32] = {0};
	char dev_name[42] = {0};
	int i = 0, j = 0;

	sprintf(cmd, "%s", "P2P_PEER FIRST");
	cmd[CMD_MAX_LEN - 1] = '\0';
	while (command_p2p_peers(cmd, addr, dev_name) == WMG_STATUS_SUCCESS) {
		if (*(results_para->num) < results_para->arr_size) {
			WMG_DEBUG("P2P_DEV: %s(%s)\n",dev_name, addr);
			strcpy(results_para->peers[j].dev_name, dev_name);
			strcpy(addr_save, addr);
			pch = strtok(addr_save, ":");  /* strtok the original string  will change into the first substringaddr*/
			for (i = 0;(pch != NULL) && (i < 6); i++) {
				results_para->peers[j].mac_addr[i] = char2uint8(pch);
				pch = strtok(NULL, ":");
			}
			(*(results_para->num))++;
			j++;
		}
		sprintf(cmd, "P2P_PEER NEXT-%s", addr);
		cmd[CMD_MAX_LEN - 1] = '\0';
	}

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_p2p_connect(uint8_t *p2p_mac_addr)
{
	wmg_status_t ret;
	int ret_tmp = -1;
	int try_cnt = 0;
	int event = -1;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	char p2p_mac[18] = {0};
	p2p_dev_private_t *p2p_dev_private = (p2p_dev_private_t *)p2p_inf_object.p2p_private_data;

	sprintf(p2p_mac, "%02x:%02x:%02x:%02x:%02x:%02x",
			p2p_mac_addr[0], p2p_mac_addr[1], p2p_mac_addr[2], p2p_mac_addr[3], p2p_mac_addr[4], p2p_mac_addr[5]);
	if(p2p_dev_private->p2p_go_intent != -1) {
		sprintf(cmd, "P2P_CONNECT %s pbc go_intent=%d", p2p_mac, p2p_dev_private->p2p_go_intent);
	} else {
		sprintf(cmd, "P2P_CONNECT %s pbc", p2p_mac);
	}
	cmd[CMD_MAX_LEN - 1] = '\0';
	ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
	if (ret) {
		WMG_ERROR("wpa_supplicant p2p connect %s failed, reply %s\n", p2p_mac, reply);
		return WMG_STATUS_FAIL;
	}

	ret_tmp = evt_socket_clear(p2p_inf_object.p2p_event_handle);
	if (ret_tmp)
		WMG_WARNG("failed to clear event socket\n");

read_event:
	ret = WMG_STATUS_FAIL;
	/* wait p2p connect event */
	while (try_cnt < EVENT_TRY_MAX) {
		ret_tmp = evt_read(p2p_inf_object.p2p_event_handle, &event);
		if (ret_tmp > 0) {
			if (event == WIFI_P2P_GO_NEG_FAILURE) {
				ret = WMG_STATUS_FAIL;
				break;
			} else if (event == WIFI_P2P_GROUP_FOR_FAILURE) {
				ret = WMG_STATUS_FAIL;
				break;
			} else if (event == WIFI_P2P_GROUP_DHCP_DNS_FAILURE) {
				ret = WMG_STATUS_FAIL;
				break;
			} else if (event == WIFI_P2P_GROUP_DHCP_FAILURE) {
				ret = WMG_STATUS_FAIL;
				break;
			} else if (event == WIFI_P2P_GROUP_DNS_FAILURE) {
				ret = WMG_STATUS_FAIL;
				break;
			} else if (event == WIFI_P2P_GROUP_DHCP_SUCCESS) {
				ret = WMG_STATUS_SUCCESS;
				break;
			} else if (event == WIFI_P2P_GROUP_DNS_SUCCESS) {
				ret = WMG_STATUS_SUCCESS;
				break;
			} else if (event == WIFI_SCAN_RESULTS) {
				ret = WMG_STATUS_UNHANDLED;
			} else {
				/* other event */
				try_cnt++;
			}
		} else {
			try_cnt++;
		}
	}

	return ret;
}

static wmg_status_t linux_p2p_disconnect(uint8_t *p2p_mac_addr)
{
	wmg_status_t ret;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	p2p_dev_private_t *p2p_dev_private = (p2p_dev_private_t *)p2p_inf_object.p2p_private_data;

	if(p2p_inf_object.p2p_role != P2P_ROLE_NON) {
		sprintf(cmd, "P2P_GROUP_REMOVE %s",p2p_dev_private->p2p_inf);
		cmd[CMD_MAX_LEN - 1] = '\0';
		ret = linux_command_to_supplicant(cmd, reply, sizeof(reply));
		if (ret) {
			WMG_ERROR("wpa_supplicant p2p group remove %s failed, reply %s\n", p2p_dev_private->p2p_inf, reply);
			return WMG_STATUS_FAIL;
		}
		//if(p2p_inf_object.p2p_role == P2P_ROLE_GO) {
		//	wmg_system("pidof dnsmasq | xargs kill -9", NULL, 0);
		//}
	}

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t wpa_parse_p2p_gp_status(char *status_buf, wifi_wpa_p2p_gp_status_t *p2p_gp_status)
{
	char *pch = NULL;
	char utf8_tmp_buf[SSID_MAX_LEN * 4] = {0};

	if (status_buf == NULL || p2p_gp_status == NULL) {
		WMG_ERROR("invalid parameters\n");
		return WMG_STATUS_INVALID;
	}

	WMG_DEBUG("wpa status reply: %s\n", status_buf);
	pch = strtok(status_buf, "'\n'");
	while (pch != NULL) {
		if (strncmp(pch, "bssid=", 6) == 0) {
			strcpy(p2p_gp_status->bssid, (pch + 6));
			WMG_DEBUG("%s\n", p2p_gp_status->bssid);
		}
		if (strncmp(pch, "freq=", 5) == 0) {
			strcpy(p2p_gp_status->freq, (pch + 5));
			WMG_DEBUG("%s\n", p2p_gp_status->freq);
		}
		if (strncmp(pch, "ssid=", 5) == 0) {
			memset(utf8_tmp_buf, '\0', SSID_MAX_LEN * 4);
			if(((strlen(pch) - 5) <= (SSID_MAX_LEN * 4)) &&
				(convert_utf8((pch + 5), utf8_tmp_buf) <= SSID_MAX_LEN)){
				strcpy(p2p_gp_status->ssid, utf8_tmp_buf);
				WMG_DEBUG("%s\n", p2p_gp_status->ssid);
			} else {
				WMG_WARNG("ssid is too long(%d)\n", (strlen(pch) - 5));
				return WMG_STATUS_INVALID;
			}
		}
		if (strncmp(pch, "id=", 5) == 0) {
			strcpy(p2p_gp_status->freq, (pch + 5));
			WMG_DEBUG("%s\n", p2p_gp_status->freq);
		}
		if (strncmp(pch, "mode=", 5) == 0) {
			strcpy(p2p_gp_status->mode, (pch + 5));
			WMG_DEBUG("%s\n", p2p_gp_status->mode);
		}
		if (strncmp(pch, "pairwise_cipher=", 16) == 0) {
			strcpy(p2p_gp_status->pairwise_cipher, (pch + 16));
			WMG_DEBUG("%s\n", p2p_gp_status->pairwise_cipher);
		}
		if (strncmp(pch, "group_cipher=", 13) == 0) {
			strcpy(p2p_gp_status->group_cipher, (pch + 13));
			WMG_DEBUG("%s\n", p2p_gp_status->group_cipher);
		}
		if (strncmp(pch, "key_mgmt=", 9) == 0) {
			strcpy(p2p_gp_status->key_mgmt, (pch + 9));
			WMG_DEBUG("%s\n", p2p_gp_status->key_mgmt);
		}
		if (strncmp(pch, "ip_address=", 11) == 0) {
			strcpy(p2p_gp_status->ip_address, (pch + 11));
			WMG_DEBUG("%s\n", p2p_gp_status->ip_address);
		}
		if (strncmp(pch, "p2p_device_address=", 19) == 0) {
			strcpy(p2p_gp_status->p2p_device_address, (pch + 19));
			WMG_DEBUG("%s\n", p2p_gp_status->p2p_device_address);
		}
		if (strncmp(pch, "address=", 8) == 0) {
			strcpy(p2p_gp_status->address, (pch + 8));
			WMG_DEBUG("%s\n", p2p_gp_status->address);
		}
		pch = strtok(NULL, "'\n'");
	}
	return WMG_STATUS_SUCCESS;
}

/* p2p forms a group and needs to obtain information from the new network port */
static wmg_status_t linux_p2p_get_gp_info(wifi_p2p_info_t *p2p_info)
{
	wmg_status_t ret;
	char cmd[CMD_MAX_LEN] = {0};
	char reply[EVENT_BUF_SIZE] = {0};
	char tmp_char[128] = {0};
	char *pch = NULL;
	int i = 0;
	wifi_wpa_p2p_gp_status_t p2p_gp_status;
	memset(&p2p_gp_status, 0, sizeof(wifi_wpa_p2p_gp_status_t));

	p2p_dev_private_t *p2p_private_data = (p2p_dev_private_t *)p2p_inf_object.p2p_private_data;

	sprintf(cmd, "STATUS");
	cmd[CMD_MAX_LEN - 1] = '\0';
	ret = command_to_wpad(cmd, reply, sizeof(reply), WIFI_P2P, p2p_private_data->p2p_inf);
	if (ret) {
		WMG_ERROR("wpa_supplicant p2p get group status fail, reply %s\n", reply);
		return WMG_STATUS_FAIL;
	} else {
		wpa_parse_p2p_gp_status(reply, &p2p_gp_status);

		memset(tmp_char, 0, 128);
		strcpy(tmp_char, p2p_gp_status.address);
		pch = strtok(tmp_char, ":");
		for(i = 0; (pch != NULL) && (i < 6); i++){
			p2p_info->bssid[i] = char2uint8(pch);
			pch = strtok(NULL, ":");
		}

		if (strncmp(p2p_gp_status.mode, "P2P GO", 6) == 0) {
			p2p_info->mode = 1;
		} else if (strncmp(p2p_gp_status.mode, "station", 7) == 0) {
			p2p_info->mode = 2;
		} else {
			p2p_info->mode = 0;
		}

		p2p_info->freq = atoi(p2p_gp_status.freq);

		memcpy(p2p_info->ssid, p2p_gp_status.ssid, SSID_MAX_LEN);

		memset(tmp_char, 0, 128);
		strcpy(tmp_char, p2p_gp_status.ip_address);
		pch = strtok(tmp_char, ".");
		for(i = 0; (pch != NULL) && (i < 4); i++){
			p2p_info->ip[i] = atoi(pch);
			pch = strtok(NULL, ".");
		}
	}

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_p2p_get_info(wifi_p2p_info_t *p2p_info)
{
	if(p2p_info == NULL) {
		WMG_ERROR("p2p info is NULL\n");
		return WMG_STATUS_FAIL;
	}

	p2p_dev_private_t *p2p_private_data = (p2p_dev_private_t *)p2p_inf_object.p2p_private_data;

	if(p2p_private_data->role == P2P_ROLE_NON) {
		memcpy(p2p_info->bssid, p2p_private_data->go_dev_addr, (sizeof(uint8_t) * 6));
		p2p_info->mode = p2p_private_data->role;
		p2p_info->freq = p2p_private_data->p2p_freq;
		memcpy(p2p_info->ssid, p2p_private_data->p2p_ssid, SSID_MAX_LEN);
	} else {
		linux_p2p_get_gp_info(p2p_info);
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_p2p_platform_extension(int cmd, void* cmd_para,int *erro_code)
{
	switch (cmd) {
		case P2P_CMD_ENABLE:
			return linux_p2p_enable((wifi_p2p_config_t *)cmd_para);
		case P2P_CMD_DISABLE:
			return linux_p2p_disable();
		case P2P_CMD_FIND:
			return linux_p2p_find(*((int *)cmd_para));
		case P2P_CMD_GET_PEERS:
			return linux_p2p_get_peers((find_results_para_t *)cmd_para);
		case P2P_CMD_CONNECT:
			return linux_p2p_connect((uint8_t *)cmd_para);
		case P2P_CMD_DISCONNECT:
			return linux_p2p_disconnect((uint8_t *)cmd_para);
		case P2P_CMD_GET_INFO:
			return linux_p2p_get_info((wifi_p2p_info_t *)cmd_para);
		default:
			return WMG_STATUS_FAIL;
	}
}

static p2p_dev_private_t p2p_dev_private = {
	.role = P2P_ROLE_NON,
	.p2p_inf = {0},
	.p2p_ssid = {0},
	.p2p_freq = 0,
	.p2p_discover_time = 0,
	.p2p_go_intent = -1,
	.p2p_psk = {0},
	.go_dev_addr = {0},
};

static wmg_p2p_inf_object_t p2p_inf_object = {
	.p2p_init_flag = WMG_FALSE,
	.p2p_enable_flag = WMG_FALSE,
	.p2p_event_cb = NULL,
	.auto_connect = WMG_FALSE,
	.p2p_role = P2P_ROLE_NON,
	.p2p_event_handle = NULL,
	.p2p_private_data = &p2p_dev_private,

	.p2p_inf_init = linux_p2p_inf_init,
	.p2p_inf_deinit = linux_p2p_inf_deinit,
	.p2p_inf_enable = linux_p2p_inf_enable,
	.p2p_inf_disable = linux_p2p_inf_disable,
	.p2p_platform_extension = linux_p2p_platform_extension,
};

wmg_p2p_inf_object_t * p2p_linux_inf_object_register(void)
{
	return &p2p_inf_object;
}
