/*
 * Copyright (C) 2017 XRADIO TECHNOLOGY CO., LTD. All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the
 *       distribution.
 *    3. Neither the name of XRADIO TECHNOLOGY CO., LTD. nor the names of
 *       its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <fcntl.h>
#include <linux/sockios.h>

#include <wifimg.h>
#include <linux_common.h>
#include <wifi_log.h>
#include <expand_cmd.h>

#define TXRX_PARA								SIOCDEVPRIVATE+1

typedef struct android_wifi_priv_cmd {
	char *buf;
	int used_len;
	int total_len;
} android_wifi_priv_cmd;

wmg_status_t wmg_linux_send_expand_cmd_ioctl(char *expand_cmd, void *expand_params, char *expand_cb)
{
	WMG_DEBUG("expand_cmd %s\n", expand_cmd);
	/* analyse input string by spaces.*/
	int loop_flag = 0;
	char *p = NULL;
	char *q = NULL;
	char *cmd_argc1 = NULL;
	char *cmd_argc2 = NULL;
	char *cmd_argc3 = NULL;
	p = expand_cmd;
	while((q = strchr(p, ' ')) != NULL){
		if(loop_flag == 0){
			*q = '\0';
			cmd_argc1 = p;
		}
		if(loop_flag == 1){
			*q = '\0';
			cmd_argc2 = p;
		}
		p = q + 1;
		loop_flag +=1;
	}
	cmd_argc3 = p;
	WMG_DEBUG("cmd_argc1 = %s, cmd_argc2 = %s, cmd_argc3 = %s\n", cmd_argc1, cmd_argc2, cmd_argc3);

#if 1
	/* send ioctl cmd.*/
	int sock;
	struct ifreq ifr;
	char buf[DATA_BUF_SIZE];
	struct android_wifi_priv_cmd priv_cmd;
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		WMG_ERROR("bad sock!\n");
		return WMG_STATUS_FAIL;
	}

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, cmd_argc1);

	if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
		WMG_ERROR("Could not read interface %s flags: %s", cmd_argc1, strerror(errno));
		close(sock);
		return WMG_STATUS_FAIL;
	}

	if (!(ifr.ifr_flags & IFF_UP)) {
		WMG_ERROR("%s is not up!\n", cmd_argc1);
		close(sock);
		return WMG_STATUS_FAIL;
	}

	memset(&priv_cmd, 0, sizeof(priv_cmd));
	memset(buf, 0, sizeof(buf));

	strcat(buf, cmd_argc2);
	strcat(buf, " ");
	strcat(buf, cmd_argc3);

    WMG_DEBUG("buf: %s \n", buf);
	priv_cmd.buf = buf;
	priv_cmd.used_len = strlen(buf);
	priv_cmd.total_len = sizeof(buf);
	ifr.ifr_data = (void*)&priv_cmd;

    if ((ioctl(sock, TXRX_PARA, &ifr)) < 0) {
        WMG_ERROR("error ioctl[TX_PARA] \n");
        close(sock);
        return WMG_STATUS_FAIL;
    }
#endif
    close(sock);
    return WMG_STATUS_SUCCESS;
}

wmg_status_t wmg_linux_send_expand_cmd_shell(char *expand_cmd, void *expand_params, void *expand_cb)
{
	wmg_system(expand_cmd, NULL, 0);
	return WMG_STATUS_SUCCESS;
}

wmg_status_t wmg_linux_send_expand_cmd_gmac(char *expand_cmd, void *expand_params, void *expand_cb)
{
	int sock_mac = -1;
	char *pch;
	int i;
	char ifname[IFNAMSIZ] = {0};
	struct ifreq ifr_mac;
	char mac_addr_buf[32];
	uint8_t *mac_addr = (uint8_t *)expand_cb;

	if(expand_cmd == NULL || expand_cmd[0] == '\0' || strlen(expand_cmd) >= sizeof(ifname)) {
		WMG_ERROR("invalid interface name for gmac\n");
		return WMG_STATUS_FAIL;
	}
	snprintf(ifname, sizeof(ifname), "%s", expand_cmd);

	sock_mac = socket(AF_INET, SOCK_STREAM, 0);
	if(sock_mac == -1)
	{
		WMG_ERROR("create mac socket faile\n");
		return WMG_STATUS_FAIL;
	}

	memset(&ifr_mac,0,sizeof(ifr_mac));
	strncpy(ifr_mac.ifr_name, ifname, (sizeof(ifr_mac.ifr_name) - 1));
	if((ioctl(sock_mac, SIOCGIFHWADDR, &ifr_mac)) < 0) {
		WMG_ERROR("mac ioctl(ifname:%s) error\n", ifname);
		close(sock_mac);
		return WMG_STATUS_FAIL;
	}

	sprintf(mac_addr_buf,"%02x:%02x:%02x:%02x:%02x:%02x",
			(unsigned char)ifr_mac.ifr_hwaddr.sa_data[0],
			(unsigned char)ifr_mac.ifr_hwaddr.sa_data[1],
			(unsigned char)ifr_mac.ifr_hwaddr.sa_data[2],
			(unsigned char)ifr_mac.ifr_hwaddr.sa_data[3],
			(unsigned char)ifr_mac.ifr_hwaddr.sa_data[4],
			(unsigned char)ifr_mac.ifr_hwaddr.sa_data[5]);

	pch = strtok(mac_addr_buf, ":");
	for(i = 0;(pch != NULL) && (i < 6); i++){
		mac_addr[i] = char2uint8(pch);
		pch = strtok(NULL, ":");
	}

	WMG_DEBUG("local mac:%s\n",mac_addr_buf);

	close(sock_mac);
	return WMG_STATUS_SUCCESS;
}

wmg_status_t wmg_linux_send_expand_cmd_smac(char *expand_cmd, void *expand_params, void *expand_cb)
{
	int sock_mac = -1;
	struct ifreq ifr_mac;
	int i = 0;
	uint8_t mac_addr[6] = {0};
	char ifname[IFNAMSIZ] = {0};
	char *pch;

	if(expand_cmd == NULL) {
		WMG_ERROR("invalid interface name for smac\n");
		return WMG_STATUS_FAIL;
	}
	pch = strtok(expand_cmd, ":");
	if(pch == NULL || strlen(pch) >= sizeof(ifname)) {
		WMG_ERROR("invalid interface name for smac\n");
		return WMG_STATUS_FAIL;
	}
	snprintf(ifname, sizeof(ifname), "%s", pch);
	pch = strtok(NULL, ":");
	if(pch == NULL || pch[0] == '\0') {
		WMG_ERROR("mac address format is incorrect\n");
		return WMG_STATUS_FAIL;
	}
	pch++;
	for(i = 0;(pch != NULL) && (i < 6); i++){
		mac_addr[i] = char2uint8(pch);
		pch = strtok(NULL, ":");
	}

	if(i != 6) {
		WMG_ERROR("%s: mac address format is incorrect\n", expand_cmd);
		return WMG_STATUS_FAIL;
	}

	/***** down the network *****/
	WMG_INFO("ioctl %s down\n", ifname);
	sock_mac = socket(AF_INET, SOCK_STREAM, 0);
	if(sock_mac == -1) {
		WMG_ERROR("down network(%s): create mac socket faile\n", ifname);
		return WMG_STATUS_FAIL;
	}
	strncpy(ifr_mac.ifr_name, ifname, (sizeof(ifr_mac.ifr_name) - 1));
	ifr_mac.ifr_flags &= ~IFF_UP;  //ifconfig   donw
	if((ioctl(sock_mac, SIOCSIFFLAGS, &ifr_mac)) < 0) {
		WMG_ERROR("down network(%s): mac ioctl error\n", ifname);
		close(sock_mac);
		return WMG_STATUS_FAIL;
	}
	close(sock_mac);
	sock_mac = -1;
	WMG_DEBUG("wait 3 second\n");
	sleep(3);

	/***** set mac addr to network *****/
	WMG_INFO("ioctl set %s mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
			ifname, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
	sock_mac = socket(AF_INET, SOCK_STREAM, 0);
	ifr_mac.ifr_addr.sa_family = ARPHRD_ETHER;
	strcpy(ifr_mac.ifr_name, ifname);

	for(i=0; i<6; i++) {
		ifr_mac.ifr_hwaddr.sa_data[i]= (char)mac_addr[i];
	}
	if((ioctl(sock_mac, SIOCSIFHWADDR, &ifr_mac)) < 0) {
		WMG_ERROR("set network(%s): mac ioctl error\n", ifname);
		close(sock_mac);
		return WMG_STATUS_FAIL;
	}
	close(sock_mac);
	sock_mac = -1;

	/***** up the network *****/
	WMG_INFO("ioctl %s up\n", ifname);
	sock_mac = socket(AF_INET, SOCK_STREAM, 0);
	strncpy(ifr_mac.ifr_name, ifname, (sizeof(ifr_mac.ifr_name) - 1));
	ifr_mac.ifr_flags |= IFF_UP;    // ifconfig   up
	if((ioctl(sock_mac, SIOCSIFFLAGS, &ifr_mac)) < 0) {
		WMG_ERROR("up network(%s): mac ioctl error\n", ifname);
		close(sock_mac);
		return WMG_STATUS_FAIL;
	}

	close(sock_mac);
	return WMG_STATUS_SUCCESS;
}

#define GCCODE_BUF_SIZE 18
wmg_status_t wmg_linux_send_expand_cmd_gccode(char *expand_cmd, void *expand_params, void *expand_cb)
{
	char cmd_log_buf[GCCODE_BUF_SIZE] = {0};
	wmg_system("iw reg get", cmd_log_buf, GCCODE_BUF_SIZE);
	WMG_DEBUG("get country code : %s\n", &cmd_log_buf[GCCODE_BUF_SIZE -3]);
	sprintf((char *)expand_cb, "%s", &cmd_log_buf[GCCODE_BUF_SIZE -3]);
	return WMG_STATUS_SUCCESS;
}

wmg_status_t wmg_linux_send_expand_cmd_sccode(char *expand_cmd, void *expand_params, void *expand_cb)
{
	char cmd_log_buf[GCCODE_BUF_SIZE] = {0};
	char cmd[128] = {0};

	wmg_system("iw reg reload", NULL, 0);
	usleep(5000);

	sprintf(cmd, "iw reg set %s", expand_cmd);
	wmg_system(cmd, NULL, 0);
	sleep(1);

	wmg_system("iw reg get", cmd_log_buf, GCCODE_BUF_SIZE);
	if (!strncmp(&cmd_log_buf[GCCODE_BUF_SIZE -3], expand_cmd, 2)) {
		return WMG_STATUS_SUCCESS;
	}

	return WMG_STATUS_FAIL;
}

wmg_status_t wmg_linux_send_expand_cmd(char *expand_cmd, void *expand_params, void *expand_cb)
{
	WMG_DEBUG("linux get exp cmd: %s\n", expand_cmd);
	if(!strncmp(expand_cmd, "shell:", 6)) {
		return wmg_linux_send_expand_cmd_shell((expand_cmd + 6), expand_params, expand_cb);
	} else if(!strncmp(expand_cmd, "ioctl:", 6)) {
		return wmg_linux_send_expand_cmd_ioctl((expand_cmd + 6), expand_params, expand_cb);
	} else if(!strncmp(expand_cmd, "gmac:", 5)) {
		return wmg_linux_send_expand_cmd_gmac((expand_cmd + 5), expand_params, expand_cb);
	} else if(!strncmp(expand_cmd, "smac:", 5)) {
		return wmg_linux_send_expand_cmd_smac((expand_cmd + 5), expand_params, expand_cb);
	} else if(!strncmp(expand_cmd, "gccode:", 7)) {
		return wmg_linux_send_expand_cmd_gccode((expand_cmd + 7), expand_params, expand_cb);
	} else if(!strncmp(expand_cmd, "sccode:", 7)) {
		return wmg_linux_send_expand_cmd_sccode((expand_cmd + 7), expand_params, expand_cb);
	} else {
		WMG_ERROR("unspport linux expand_cmd: %s\n", expand_cmd);
	}
	return WMG_STATUS_FAIL;
}
