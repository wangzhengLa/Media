#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netinet/ether.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <wifi_log.h>
#include <wmg_monitor.h>
#include <linux_monitor.h>
#include <linux_common.h>
#include <netlink/netlink.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <netlink/msg.h>
#include <netlink/attr.h>

static wmg_monitor_inf_object_t monitor_inf_object;

static int nl80211_init(nl80211_state_t *state)
{
	int err;

	state->nl_sock = nl_socket_alloc();
	if (!state->nl_sock) {
		WMG_ERROR("failed to allocate netlink socket\n");
		return -ENOMEM;
	}

	if (genl_connect(state->nl_sock)) {
		WMG_ERROR("failed to connect to generic netlink\n");
		err = -ENOLINK;
		goto out_handle_destroy;
	}

	nl_socket_set_buffer_size(state->nl_sock, 8192, 8192);

	state->nl80211_id = genl_ctrl_resolve(state->nl_sock, "nl80211");
	if (state->nl80211_id < 0) {
		WMG_ERROR("nl80211 not found\n");
		err = -ENOENT;
		goto out_handle_destroy;
	}

	return 0;

out_handle_destroy:
	nl_socket_free(state->nl_sock);
	return err;
}

static void nl80211_cleanup(nl80211_state_t *state)
{
	nl_socket_free(state->nl_sock);
}

static int read_mon_if_from_conf(void)
{
	FILE *f = NULL;
	char buf[MON_BUF_SIZE] = {0};
	int ret = -1, file_size = 0, len = 0;
	char *pos = NULL, *pre = NULL;

	f = fopen(MON_CONF_PATH, "r");
	if (f != NULL) {
		fseek(f, 0, SEEK_END);
		file_size = (ftell(f) > MON_BUF_SIZE) ? MON_BUF_SIZE : ftell(f);
		fseek(f, 0, SEEK_SET);
		ret = fread(buf, 1, file_size, f);
		if (ret == file_size) {
			WMG_DEBUG("read wifi monitor interface from %s\n", MON_CONF_PATH);
			pos = strstr(buf, "mon_if=");
			if (pos != NULL) {
				pos += 7;
				pre = pos;
				pos = strchr(pos, '\n'); /* exist other parameters ? */
				if (pos != NULL)
					len = pos - pre;
				else
					len = strlen(pre);

				if (len != 0 && len < MON_IF_SIZE) {
					strncpy(monitor_inf_object.monitor_if, pre, len);
					WMG_DEBUG("monitor interface is '%s'\n", monitor_inf_object.monitor_if);
					ret = 0;
				} else {
					WMG_WARNG("monitor interface name maybe error\n");
				}
			}
		}

		fclose(f);
		f = NULL;
	}

	return ret;
}

static void clean_mon_recv_thread(void *arg)
{
	int *sockfd_p = (int *)arg;
	int sockfd = *sockfd_p;

	WMG_DEBUG("***** close mon recv thread sockfd:%d *****\n",sockfd);
	close(sockfd);
}

static void *mon_recv_thread(void *p)
{
	int sockfd, sockopt;
	ssize_t numbytes;
	struct ifreq ifopts;    /* set promiscuous mode */
	static unsigned char buf[MON_BUF_SIZE];
	unsigned char *pkt = NULL;
	wifi_monitor_data_t monitor_data_frame;

	WMG_DEBUG("***** Create monitor data receive thread *****\n");

	/* Create a raw socket that shall sniff */
	/* Open PF_PACKET socket, listening for EtherType ETHER_TYPE */
	if ((sockfd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) == -1) {
		WMG_ERROR("socket - %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Set interface to promiscuous mode - do we need to do this every time? */
	strncpy(ifopts.ifr_name, monitor_inf_object.monitor_if, IFNAMSIZ - 1);
	ioctl(sockfd, SIOCGIFFLAGS, &ifopts);
	ifopts.ifr_flags |= IFF_PROMISC;
	ioctl(sockfd, SIOCSIFFLAGS, &ifopts);

	/* Allow the socket to be reused - incase connection is closed prematurely */
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof sockopt) == -1) {
		WMG_ERROR("setsockopt - %s\n", strerror(errno));
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	/* Bind to device */
	if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, monitor_inf_object.monitor_if, IFNAMSIZ-1) == -1) {
		WMG_ERROR("setsocket SO_BINDTODEVICE - %s\n", strerror(errno));
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	pthread_cleanup_push(clean_mon_recv_thread, &sockfd);

	for (;;) {
		if (monitor_inf_object.monitor_enable == WMG_TRUE) {
			if(monitor_inf_object.monitor_data_frame_cb != NULL) {
				numbytes = recvfrom(sockfd, buf, MON_BUF_SIZE, 0, NULL, NULL);
				if (numbytes < 0) {
					continue;
				} else {
					WMG_DUMP("receive data, size = %d\n", numbytes);
					monitor_data_frame.data = buf;
					monitor_data_frame.len = numbytes;
					monitor_data_frame.channel = monitor_inf_object.monitor_channel;
					monitor_data_frame.info = NULL;
					monitor_inf_object.monitor_data_frame_cb(&monitor_data_frame);
				}
			}
		}
	}

	pthread_cleanup_pop(1);
	pthread_exit(NULL);
}

static int ieee80211_channel_to_frequency(int chan, enum nl80211_band band)
{
	/* see 802.11 17.3.8.3.2 and Annex J
	 * there are overlapping channel numbers in 5GHz and 2GHz bands */
	if (chan <= 0) {
		return 0; /* not supported */
	}
	switch (band) {
	case NL80211_BAND_2GHZ:
		if (chan == 14)
			return 2484;
		else if (chan < 14)
			return 2407 + chan * 5;
		break;
	case NL80211_BAND_5GHZ:
		if (chan >= 182 && chan <= 196)
			return 4000 + chan * 5;
		else
			return 5000 + chan * 5;
		break;
	case NL80211_BAND_60GHZ:
		if (chan < 5)
			return 56160 + chan * 2160;
		break;
	default:
		;
	}
	return 0; /* not supported */
}

static wmg_status_t linux_monitor_set_channel(uint8_t channel)
{
	wmg_status_t ret;
	struct nl_msg *msg;
	signed long long devidx = 0;
	uint32_t freq;
	enum nl80211_band band;
	char cmd[64];

	if (monitor_inf_object.monitor_init_flag == WMG_FALSE) {
		WMG_ERROR("wifi monitor is not initialized\n");
		return WMG_STATUS_FAIL;
	}

	if (monitor_inf_object.monitor_enable == WMG_FALSE) {
		WMG_ERROR("set monitor channel failed because wifi monitor is disabled\n");
		return WMG_STATUS_FAIL;
	}

	sprintf(cmd, "iw %s set channel %d", monitor_inf_object.monitor_if, channel);
	wmg_system(cmd, NULL, 0);

	monitor_inf_object.monitor_channel = channel;
	ret = WMG_STATUS_SUCCESS;

	WMG_DEBUG("wifi monitor set channel(%d) success\n", channel);

	return ret;
}

static wmg_status_t linux_monitor_inf_init(monitor_data_frame_cb_t monitor_data_frame_cb, void *p)
{
	wmg_status_t ret;
	if(monitor_inf_object.monitor_init_flag == WMG_FALSE) {
		WMG_INFO("linux monitor nl init now\n");
		monitor_inf_object.monitor_state = (nl80211_state_t *)malloc(sizeof(nl80211_state_t));
		if (monitor_inf_object.monitor_state != NULL) {
			ret = nl80211_init(monitor_inf_object.monitor_state);
			if (!ret) {
				WMG_DEBUG("nl80211 init success\n");
			} else {
				WMG_ERROR("failed to init nl80211\n");
				return WMG_STATUS_FAIL;
			}
		}
		if(monitor_data_frame_cb != NULL){
			monitor_inf_object.monitor_data_frame_cb = monitor_data_frame_cb;
		}
		monitor_inf_object.monitor_init_flag = WMG_TRUE;
	} else {
		WMG_INFO("linux monitor nl already init\n");
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_monitor_inf_deinit(void *p)
{
	if(monitor_inf_object.monitor_init_flag == WMG_TRUE){
		WMG_INFO("linux monitor nl deinit now\n");
		nl80211_cleanup(monitor_inf_object.monitor_state);
		monitor_inf_object.monitor_data_frame_cb = NULL;
		free(monitor_inf_object.monitor_state);
		monitor_inf_object.monitor_channel = 255;
		monitor_inf_object.monitor_init_flag = WMG_FALSE;
	} else {
		WMG_INFO("linux monitor nl already deinit\n");
	}
	return 0;
}

static wmg_status_t linux_monitor_inf_enable()
{
	wmg_status_t ret;
	ret = read_mon_if_from_conf();
	if (!ret) {
		WMG_DEBUG("get monitor interface success\n");
	} else {
		WMG_WARNG("get monitor interface failed, just use default 'wlan0'\n");
		strncpy(monitor_inf_object.monitor_if, "wlan0", 5);
	}
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_monitor_inf_disable()
{
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_monitor_enable(wifi_monitor_config_t *monitor_config)
{
	wmg_status_t ret;
	uint8_t channel = monitor_config->channel;
	struct nl_msg *msg;
	signed long long devidx = 0;
	uint32_t freq;
	enum nl80211_band band;
	char cmd[64];

	if((channel > 14) || (channel == 0)) {
		WMG_ERROR("wifi channel(%d) is out of range 1 ~ 14\n", channel);
		return WMG_STATUS_UNSUPPORTED;
	}

	sprintf(cmd, "ifconfig %s down", monitor_inf_object.monitor_if);
	wmg_system(cmd, NULL, 0);
	sleep(1);

	sprintf(cmd, "iw %s set type monitor", monitor_inf_object.monitor_if);
	wmg_system(cmd, NULL, 0);
	sleep(1);

	sprintf(cmd, "ifconfig %s up", monitor_inf_object.monitor_if);
	wmg_system(cmd, NULL, 0);
	sleep(1);

	//Switch to another channel before monitoring a channel
	if(channel != 1) {
		sprintf(cmd, "iw %s set channel 1", monitor_inf_object.monitor_if);
		wmg_system(cmd, NULL, 0);
	} else {
		sprintf(cmd, "iw %s set channel 2", monitor_inf_object.monitor_if);
		wmg_system(cmd, NULL, 0);
	}

	sprintf(cmd, "iw %s set channel %d", monitor_inf_object.monitor_if, channel);
	wmg_system(cmd, NULL, 0);
	monitor_inf_object.monitor_channel = channel;
	sleep(1);

	ret = os_net_thread_create(&monitor_inf_object.monitor_pid, NULL, mon_recv_thread, NULL, 0, 4096);
	if (ret) {
		WMG_ERROR("failed to create linux nl dnata handle thread\n");
		return WMG_STATUS_FAIL;
	}

	monitor_inf_object.monitor_enable = WMG_TRUE;

	WMG_DEBUG("wifi monitor enable success\n");

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_monitor_disable()
{
	wmg_status_t ret = WMG_STATUS_SUCCESS;
	struct nl_msg *msg;
	signed long long devidx = 0;
	char cmd[64];

	if (monitor_inf_object.monitor_init_flag == WMG_FALSE) {
		WMG_WARNG("wifi monitor is not initialized\n");
		return WMG_STATUS_UNHANDLED;
	}

	if (monitor_inf_object.monitor_enable == WMG_FALSE) {
		WMG_WARNG("wifi monitor already disabled\n");
		return WMG_STATUS_SUCCESS;
	}

	WMG_DEBUG("wifi monitor disableing...\n");

	if(monitor_inf_object.monitor_pid != -1) {
		os_net_thread_delete(&monitor_inf_object.monitor_pid);
		monitor_inf_object.monitor_pid = -1;
	}

	sprintf(cmd, "ifconfig %s down", monitor_inf_object.monitor_if);
	wmg_system(cmd, NULL, 0);
	sleep(1);

	sprintf(cmd, "iw %s set type station", monitor_inf_object.monitor_if);
	wmg_system(cmd, NULL, 0);
	sleep(1);

	monitor_inf_object.monitor_enable = WMG_FALSE;
	monitor_inf_object.monitor_channel = 255;

	WMG_DEBUG("wifi monitor disable success\n");

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t linux_platform_extension(int cmd, void* cmd_para,int *erro_code)
{
	switch (cmd) {
		case MONITOR_CMD_ENABLE:
			return linux_monitor_enable((wifi_monitor_config_t *)(cmd_para));
		case MONITOR_CMD_DISABLE:
			return linux_monitor_disable(NULL);
		case MONITOR_CMD_SET_CHANNL:
			return linux_monitor_set_channel(*(uint8_t *)(cmd_para));
		default:
			return WMG_FALSE;
	}
}

static wmg_monitor_inf_object_t monitor_inf_object = {
	.monitor_init_flag = WMG_FALSE,
	.monitor_enable = WMG_FALSE,
	.monitor_pid = -1,
	.monitor_state  = NULL,
	.monitor_data_frame_cb = NULL,
	.monitor_channel = 255,

	.monitor_inf_init = linux_monitor_inf_init,
	.monitor_inf_deinit = linux_monitor_inf_deinit,
	.monitor_inf_enable = linux_monitor_inf_enable,
	.monitor_inf_disable = linux_monitor_inf_disable,
	.monitor_platform_extension = linux_platform_extension,
};

wmg_monitor_inf_object_t * monitor_linux_inf_object_register(void)
{
	return &monitor_inf_object;
}
