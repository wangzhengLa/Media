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
		WMG_INFO("Exit sta mode simple demo\n");
		exit(0);
	}
}

int main(int argc, char *argv[])
{
	pid_t pc;
	wifi_sta_cn_para_t cn_para;
	int ret_exit = -1;

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
		goto exit;
	}

	while(1) {
		sleep(1);
	}

	ret_exit = 0;

exit:
	wifimanager_deinit();
	WMG_INFO("Exit sta mode simple demo\n");
	exit(ret_exit);
}
