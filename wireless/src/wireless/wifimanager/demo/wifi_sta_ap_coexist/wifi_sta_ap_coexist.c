#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include "wifimg.h"
#include <wifi_log.h>

#define BAND_NOME     0
#define BAND_2_4G     1
#define BAND_5G       2

static uint32_t freq_to_channel(uint32_t freq)
{
	int band;
	uint32_t channel = 0;
	if((freq >= 5180) && (freq <= 5825)){
		band = BAND_5G;
	} else if((freq >= 2407) && (freq <= 2484)){
		band = BAND_2_4G;
	} else {
		band = BAND_NOME;
	}
	switch (band) {
	case BAND_2_4G:
		if(freq == 2484) {
			channel = 14;
		} else if(freq == 2407) {
			channel = 0;
		} else if((freq <= 2472) && (freq > 2407)){
			if(((freq - 2407) % 5) == 0) {
				channel = ((freq - 2407) / 5);
			} else {
				channel = 1000;
			}
		} else {
			channel = 1000;
		}
		break;
	case BAND_5G:
		if(((freq - 5180) % 5) == 0) {
			channel = ((freq - 5180) / 5) + 36;
		} else {
			channel = 1000;
		}
		break;
	case BAND_NOME:
	default:
		channel = 1000;
		break;
	}
	return channel;
}

static uint8_t get_sta_connect_channel()
{
	uint8_t channel = 1000;
	wifi_sta_info_t wifi_sta_info;
	if(wifi_sta_get_info(&wifi_sta_info) == WMG_STATUS_SUCCESS) {
		channel = freq_to_channel(wifi_sta_info.freq);
	}
	return channel;
}

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
		WMG_INFO("Exit sta mode simple demo\n");
		exit(0);
	}
}

int main(int argc, char *argv[])
{
	int ret_exit = -1;
	pid_t pc;
	wifi_sta_cn_para_t cn_para;

	char ssid_buf[SSID_MAX_LEN + 1] = "allwinner-sta-ap-coexist";
	char psk_buf[PSK_MAX_LEN + 1] = "Aa123456";
	wifi_ap_config_t ap_config;
	memset(&ap_config, 0, sizeof(wifi_ap_config_t));
	ap_config.ssid = ssid_buf;
	ap_config.psk = psk_buf;
	ap_config.sec = WIFI_SEC_WPA2_PSK;

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

	/* open sta mode */
	if(wifi_on(WIFI_STATION)) {
		WMG_ERROR("sta mode simple demo: open sta mode failed, exit now\n");
		goto exit;
	}

	/* connect wifi */
	memset(&cn_para, 0, sizeof(wifi_sta_cn_para_t));
	cn_para.ssid = argv[1];
	cn_para.password = argv[2];
	cn_para.sec = WIFI_SEC_UNKNOWN;
	if(wifi_sta_connect(&cn_para) == 0) {
		WMG_INFO("==Wi-Fi ssid: %s password: %s sec: %d connect successful==\n",
				cn_para.ssid, cn_para.password, cn_para.sec);
	} else {
		WMG_ERROR("==Wi-Fi connect failed==\n");
		return -1;
	}

	/* Get the channel after the STA mode connection is successful */
	ap_config.channel = get_sta_connect_channel();
	if(ap_config.channel != 1000) {
		WMG_INFO("wifi connect ap channel %d\n", ap_config.channel);
	} else {
		wifi_off(WIFI_MODE_UNKNOWN);
		goto exit;
	}

	/* coexistence mode opens ap */
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
	WMG_INFO("Exit sta ap coexist simple demo\n");
	exit(ret_exit);
}
