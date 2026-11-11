#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include "wifimg.h"
#include <wifi_log.h>

static uint8_t char2uint8(char* trs)
{
	uint8_t ret = 0;
	uint8_t tmp_ret[2] = {0};
	int i = 0;
	for(; i < 2; i++) {
		switch (*(trs + i)) {
			case '0': tmp_ret[i] = 0x0; break;
			case '1': tmp_ret[i] = 0x1; break;
			case '2': tmp_ret[i] = 0x2; break;
			case '3': tmp_ret[i] = 0x3; break;
			case '4': tmp_ret[i] = 0x4; break;
			case '5': tmp_ret[i] = 0x5; break;
			case '6': tmp_ret[i] = 0x6; break;
			case '7': tmp_ret[i] = 0x7; break;
			case '8': tmp_ret[i] = 0x8; break;
			case '9': tmp_ret[i] = 0x9; break;
			case 'a': case 'A': tmp_ret[i] = 0xa; break;
			case 'b': case 'B': tmp_ret[i] = 0xb; break;
			case 'c': case 'C': tmp_ret[i] = 0xc; break;
			case 'd': case 'D': tmp_ret[i] = 0xd; break;
			case 'e': case 'E': tmp_ret[i] = 0xe; break;
			case 'f': case 'F': tmp_ret[i] = 0xf; break;
		}
	}
	ret = ((tmp_ret[0] << 4) | tmp_ret[1]);
	return ret;
}

#ifndef UNREGISTER_CB
//Callback functions cannot do high-load and blocking actions
void wifi_msg_cb(wifi_msg_data_t *msg)
{
	if(msg->id == WIFI_MSG_ID_P2P_STATE_CHANGE) {
		WMG_DEBUG("get p2p state:(%d) ", msg->data.p2p_state);
		switch(msg->data.p2p_state) {
			case WIFI_P2P_ENABLE:
			WMG_INFO("p2p state: enable\n");
			break;
			case WIFI_P2P_DISABLE:
			WMG_INFO("p2p state: disable\n");
			break;
			case WIFI_P2P_DISCOVER:
			WMG_INFO("p2p state: discovering...\n");
			break;
			case WIFI_P2P_CONNECTD_GC:
			WMG_INFO("p2p state: connectd gc\n");
			break;
			case WIFI_P2P_CONNECTD_GO:
			WMG_INFO("p2p state: connectd go\n");
			break;
			case WIFI_P2P_DISCONNECTD:
			WMG_INFO("p2p state: disconnected\n");
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
		WMG_INFO("Exit p2p mode simple demo\n");
		exit(0);
	}
}

#define DEFAULT_P2P_NUM 5
int main(int argc, char *argv[])
{
	pid_t pc;
	int ret_exit = -1, i = 0, p2p_num = 0;
	wifi_p2p_config_t p2p_config;
	memset(&p2p_config, 0, sizeof(wifi_p2p_config_t));
	char p2p_dev_buf[] = "allwinner-p2p-demo-dev";
	/* listen 10 second to connect */
	p2p_config.discover_time = 10;
	p2p_config.p2p_go_intent = 7;
	p2p_config.auto_connect = WMG_TRUE;
	char *pch;
	uint8_t p2p_mac[6] = {0};
	wifi_p2p_peers_t p2p_peers[DEFAULT_P2P_NUM] = {0};

	memset(&p2p_peers, 0, (sizeof(wifi_p2p_peers_t) * DEFAULT_P2P_NUM));

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

	/* open p2p mode */
	if(wifi_on(WIFI_P2P)) {
		WMG_ERROR("p2p mode simple demo: open p2p mode failed, exit now\n");
		goto exit;
	}

	/* enable p2p mode */
	if(wifi_p2p_enable(&p2p_config)) {
		WMG_ERROR("p2p mode simple demo: p2p mode enable failed, exit now\n");
		goto exit;
	}

	if(argv[1] == NULL) {
		WMG_INFO("p2p demo: discover 10 second\n");
		wifi_p2p_find(10);
		sleep(10);
		wifi_p2p_get_peers(p2p_peers, &p2p_num, DEFAULT_P2P_NUM);
		WMG_INFO("=============================\n", p2p_num);
		WMG_INFO("get %d p2p peers\n", p2p_num);
		for (i = 0; i < p2p_num; i++) {
			WMG_INFO("p2p dev name: %s mac: %02x:%02x:%02x:%02x:%02x:%02x\n", p2p_peers[i].dev_name,
			p2p_peers[i].mac_addr[0], p2p_peers[i].mac_addr[1] ,p2p_peers[i].mac_addr[2],
			p2p_peers[i].mac_addr[3], p2p_peers[i].mac_addr[4] ,p2p_peers[i].mac_addr[5]);
		}
		WMG_INFO("=============================\n", p2p_num);
		ret_exit = 0;
		wifi_off(WIFI_MODE_UNKNOWN);
		goto exit;
	} else {
		pch = strtok(argv[1], ":");
		for(i = 0;(pch != NULL) && (i < 6); i++){
			p2p_mac[i] = char2uint8(pch);
			pch = strtok(NULL, ":");
		}
		/* connect p2p dev */
		if(wifi_p2p_connect(&p2p_mac) == 0) {
			WMG_INFO("==Wi-Fi p2p connect success\n");
		} else {
			WMG_ERROR("==Wi-Fi p2p connect failed==\n");
			goto exit;
		}
	}

	while(1) {
		sleep(1);
	}

	ret_exit = 0;
	wifi_off(WIFI_MODE_UNKNOWN);

exit:
	wifimanager_deinit();
	WMG_INFO("Exit p2p mode simple demo\n");
	exit(ret_exit);
}
