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
	if(msg->id == WIFI_MSG_ID_AP_STATE_CHANGE) {
		WMG_DEBUG("get ap state:(%d) ", msg->data.ap_state);
		switch(msg->data.ap_state) {
			case WIFI_AP_DISABLE:
			WMG_INFO("ap disable\n");
			break;
			case WIFI_AP_ENABLE:
			WMG_INFO("ap enable\n");
			break;
			case WIFI_AP_STA_CONNECTE:
			WMG_INFO("one sta connected\n");
			break;
			case WIFI_AP_STA_DISCONNECTED:
			WMG_INFO("one sta disconnected\n");
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
		WMG_INFO("Exit ap mode simple demo\n");
		exit(0);
	}
}

int main(int argc, char *argv[])
{
	int ret_exit = -1;
	pid_t pc;

	char ssid_buf[SSID_MAX_LEN + 1] = "allwinner-ap";
	char psk_buf[PSK_MAX_LEN + 1] = "Aa123456";
	wifi_ap_config_t ap_config;
	memset(&ap_config, 0, sizeof(wifi_ap_config_t));
	ap_config.ssid = ssid_buf;
	ap_config.psk = psk_buf;
	ap_config.sec = WIFI_SEC_WPA2_PSK;
	ap_config.channel = 6;

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

	/* for debug */
	//wmg_set_debug_level(WMG_MSG_DEBUG);

	/* register callback function */
	if(wifi_register_msg_cb(wifi_msg_cb, NULL)) {
		WMG_ERROR("register msg cb failed\n");
	}

	if(argc > 1){
		memset(ap_config.ssid, 0, SSID_MAX_LEN + 1);
		strncpy(ap_config.ssid, argv[1], SSID_MAX_LEN);
	}
	if(argc > 2){
		memset(ap_config.psk, 0, PSK_MAX_LEN + 1);
		strncpy(ap_config.psk, argv[2], SSID_MAX_LEN);
	}

	/* open ap mode */
	if(wifi_on(WIFI_AP) == 0) {
		if(wifi_ap_enable(&ap_config) == 0) {
			WMG_INFO("ap enable success, ssid=%s, psk=%s, sec=%d, channel=%d\n", ap_config.ssid,
					ap_config.psk, ap_config.sec, ap_config.channel);
		} else {
			WMG_ERROR("ap enable failed\n");
			wifi_off(WIFI_MODE_UNKNOWN);
			goto exit;
		}
	} else {
		WMG_ERROR("wifi on ap failed\n");
		wifi_off(WIFI_MODE_UNKNOWN);
		goto exit;
	}

	while(1) {
		sleep(1);
	}

	ret_exit = 0;
	wifi_off(WIFI_MODE_UNKNOWN);

exit:
	wifimanager_deinit();
	WMG_INFO("Exit ap simple demo\n");
	exit(ret_exit);
}
