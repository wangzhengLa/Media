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
	static int monitor_data_count = 0;
	static uint64_t monitor_total_data_len =0;

	if(msg->id == WIFI_MSG_ID_MONITOR) {
		switch(msg->data.mon_state) {
			case WIFI_MONITOR_DISABLE:
			WMG_INFO("monitor disable\n");
			break;
			case WIFI_MONITOR_ENABLE:
			WMG_INFO("monitor enable\n");
			break;
			default:
				monitor_total_data_len += msg->data.frame->len;
				monitor_data_count++;
				if(monitor_data_count > 1000) {
					WMG_INFO("** wmg cb: channel(%d)\n", msg->data.frame->channel);
					WMG_INFO("** data total len %llu\n", monitor_total_data_len);
					monitor_data_count = 0;
				}
			break;
		}
	}
}
#endif

void handler(int sig)
{
	WMG_INFO("get a sig, num is %d\n",sig);
	if(sig == 2){
		wifi_monitor_disable();
		wifi_off(WIFI_MODE_UNKNOWN);
		wifimanager_deinit();
		WMG_INFO("Exit mon mode simple demo\n");
		exit(0);
	}
}

int main(int argc, char *argv[])
{
	int ret_exit = -1;
	pid_t pc;

	wifi_monitor_config_t monitor_config;
	memset(&monitor_config, 0, sizeof(wifi_monitor_config_t));
	monitor_config.channel = 6;

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
		monitor_config.channel= atoi(argv[1]);
	}

	/* open monitor mode */
	if(wifi_on(WIFI_MONITOR) == WMG_STATUS_SUCCESS) {
		WMG_INFO("wifi on mon success\n");
		if(wifi_monitor_enable(&monitor_config) == WMG_STATUS_SUCCESS) {
			WMG_INFO("mon enable success, channel=%d, frame num: %d\n",
				monitor_config.channel, monitor_config.printf_frame);
	} else {
		WMG_ERROR("mon enable failed\n");
		goto exit;
	}

	} else {
		WMG_ERROR("wifi on mon failed\n");
		goto exit;
	}

	while(1) {
		sleep(1);
	}

	ret_exit = 0;
	wifi_monitor_disable();
	wifi_off(WIFI_MODE_UNKNOWN);

exit:
	wifimanager_deinit();
	WMG_INFO("Exit monitor simple demo\n");
	exit(ret_exit);
}
