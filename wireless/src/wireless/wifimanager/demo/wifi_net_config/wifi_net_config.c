#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include "wifimg.h"
#include <wifi_log.h>

#ifndef UNREGISTER_CB
//Callback functions cannot do high-load and blocking actions
void wifi_msg_cb(wifi_msg_data_t *msg)
{
	if(msg->id == WIFI_MSG_ID_STA_STATE_CHANGE) {
		WMG_DEBUG("get sta state:(%d) ", msg->data.state);
		switch(msg->data.state) {
			case WIFI_STA_CONNECTING:
			WMG_INFO("Connecting......\n");
			break;
			case WIFI_STA_CONNECTED:
			WMG_INFO("Connected to the AP\n");
			break;
			case WIFI_STA_OBTAINING_IP:
			WMG_INFO("Obtaining ip address......\n");
			break;
			case WIFI_STA_NET_CONNECTED:
			WMG_DEBUG("Successful net connected\n");
			break;
			case WIFI_STA_DISCONNECTED:
			WMG_INFO("Disconnected\n");
			break;
		}
	}
}
#endif

void handler(int sig)
{
	WMG_INFO("get a sig, num is %d\n",sig);
	if(sig == 2){
		wifi_off(WIFI_MODE_UNKNOWN);
		wifimanager_deinit();
		WMG_INFO("Exit net config simple demo\n");
		exit(0);
	}
}

int main(int argc, char *argv[])
{
	int ret_exit = -1;
	pid_t pc;
	wifi_sta_cn_para_t cn_para;
	memset(&cn_para, 0, sizeof(wifi_sta_cn_para_t));
	char ssid_buf[SSID_MAX_LEN + 1] = {0};
	char psk_buf[PSK_MAX_LEN + 1] = {0};
	wifi_linkd_result_t linkd_result;
	void *linkd_params = NULL;
	wifi_linkd_mode_t linkd_mode = WMG_LINKD_MODE_NONE;

	linkd_result.ssid = ssid_buf;
	linkd_result.psk = psk_buf;

	wifi_ap_config_t ap_config;
	char ap_ssid_buf[SSID_MAX_LEN + 1] = "Aw-config-net-Test";
	char ap_psk_buf[PSK_MAX_LEN + 1] = "Aa123456";

	pc = fork();
	if(pc < 0) {
		WMG_ERROR("fork error\n");
		exit(-1);
	} else if (pc>0) {
		exit(0);
	}

	signal(2,handler);

	/* wifimanager init */
	wifimanager_init();
	/* register callback function */
	if(wifi_register_msg_cb(wifi_msg_cb, NULL)) {
		WMG_ERROR("register msg cb failed\n");
	}

	/* get network information */
	if(!strncmp(argv[1], "ble", 3)) {
		linkd_mode = WMG_LINKD_MODE_BLE;
	} else if(!strncmp(argv[1], "softap", 6)) {
		ap_config.ssid = ap_ssid_buf;
		ap_config.psk = ap_psk_buf;
		ap_config.sec = WIFI_SEC_WPA2_PSK;
		ap_config.channel = 6;
		linkd_params = &ap_config;
		linkd_mode = WMG_LINKD_MODE_SOFTAP;
	} else {
		WMG_ERROR("Unsupport net config\n");
		goto exit;
	}

	if(wifi_linkd_protocol(linkd_mode, linkd_params, 120, &linkd_result) != WMG_STATUS_SUCCESS){
		WMG_ERROR("config net get result failed\n");
		goto exit;
	} else {
		cn_para.ssid = ssid_buf;
		cn_para.password = psk_buf;
	}

	/* open sta mode */
	if(wifi_on(WIFI_STATION)) {
		WMG_ERROR("net config simple demo: open sta mode failed, exit now\n");
		goto exit;
	}

	/* connect wifi */
	cn_para.sec = WIFI_SEC_UNKNOWN;
	if(wifi_sta_connect(&cn_para) == 0) {
		WMG_INFO("==Wi-Fi ssid: %s password: %s sec: %d connect successful==\n",
			cn_para.ssid, cn_para.password, cn_para.sec);
	} else {
		WMG_ERROR("==Wi-Fi connect failed==\n");
		goto exit;
	}

	while(1) {
		sleep(1);
	}

	ret_exit = 0;

exit:
	wifimanager_deinit();
	WMG_INFO("Exit net config simple demo\n");
	exit(ret_exit);
}
