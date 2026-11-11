
#include <wmg_sta.h>
#include <wifi_log.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <net/if_arp.h>
#include <wpa_ctrl.h>
#include <linux_common.h>
#include <wmg_get_config.h>
#include <linux_get_config.h>
#include <linux_support_list.h>
#include <linux_sta.h>
#include <linux_p2p.h>
#include <linux/event.h>
#include <utils.h>
#include <errno.h>
#include <poll.h>
#include <udhcpc.h>
#include <pthread.h>

uint8_t char2uint8(char* trs)
{
	uint8_t ret = 0;
	uint8_t tmp_ret[2] = {0};
	int i = 0;
	for(; i < 2; i++) {
		switch (*(trs + i)) {
			case '0' :
				tmp_ret[i] = 0x0;
				break;
			case '1' :
				tmp_ret[i] = 0x1;
				break;
			case '2' :
				tmp_ret[i] = 0x2;
				break;
			case '3' :
				tmp_ret[i] = 0x3;
				break;
			case '4' :
				tmp_ret[i] = 0x4;
				break;
			case '5' :
				tmp_ret[i] = 0x5;
				break;
			case '6' :
				tmp_ret[i] = 0x6;
				break;
			case '7' :
				tmp_ret[i] = 0x7;
				break;
			case '8' :
				tmp_ret[i] = 0x8;
				break;
			case '9' :
				tmp_ret[i] = 0x9;
				break;
			case 'a' :
			case 'A' :
				tmp_ret[i] = 0xa;
				break;
			case 'b' :
			case 'B' :
				tmp_ret[i] = 0xb;
				break;
			case 'c' :
			case 'C' :
				tmp_ret[i] = 0xc;
				break;
			case 'd' :
			case 'D' :
				tmp_ret[i] = 0xd;
				break;
			case 'e' :
			case 'E' :
				tmp_ret[i] = 0xe;
				break;
			case 'f' :
			case 'F' :
				tmp_ret[i] = 0xf;
		break;
		}
	}
	ret = ((tmp_ret[0] << 4) | tmp_ret[1]);
	return ret;
}

static char * wifi_mode_to_char(wifi_mode_t wifi_mode)
{
	if(wifi_mode & WIFI_STATION) {
		return "STA";
	} else if(wifi_mode & WIFI_AP) {
		return "AP";
	} else if(wifi_mode & WIFI_P2P) {
		return "P2P";
	}
	return "UNKNOWN";
}

void wmg_system(const char* cmd, char *output, int size)
{
	FILE *fp = NULL;
	int offset = 0, len = 0;

	if ((output != NULL) && (size > 0)) {
		fp = popen(cmd, "r");
		if (fp) {
			while (((size - 1) > offset) && (fgets(output + offset, size - offset, fp) != NULL)) {
				len = strlen(output + offset);
				offset += len;
			}
			pclose(fp);
		} else {
			WMG_ERROR("popen cmd: %s executed failed\n", cmd);
		}
	} else {
		if(system(cmd) == 0) {
			WMG_DEBUG("system cmd: %s executed successfully\n", cmd);
		} else {
			WMG_ERROR("system cmd: %s executed failed\n", cmd);
		}
	}

	return;
}

#define WPAD_STA_BITMAP    0x1
#define WPAD_P2P_BITMAP    0x2
#define WPAD_AP_BITMAP     0x4

#define TRY_TIMES   3

static wmg_status_t update_wpad_indep_bitmap(uint8_t *bitmap)
{
	uint8_t tmp_bitmap = 0;
	if((tmp_bitmap = get_linux_current_module_indep_bitmap()) < 7) {
		WMG_DEBUG("get wpad indep bitmap %d, update bitmap now\n", tmp_bitmap);
		*bitmap = tmp_bitmap;
	} else {
		WMG_ERROR("wpad indep config illegal(%d)\n", tmp_bitmap);
		WMG_WARNG("get wpad indep bitmap fail, use default %d\n", *bitmap);
	}
	return WMG_STATUS_SUCCESS;
}

/*When modifying this macro, you need to pay attention to whether it overflows.
 *some type num unuse, so this macro can only save pointers, otherwise it will cause memory waste
 */
#define WIFI_MODE_TYPE_NUM  16
typedef struct {
	char wifi_module_name[MODULE_NAME_MAX_LEN];
	bool update_wpad_config_flag;
	pthread_mutex_t wpa_supplicant_mutex;
	struct wpa_ctrl *ctrl_conn[WIFI_MODE_TYPE_NUM];
	struct wpa_ctrl *monitor_conn[WIFI_MODE_TYPE_NUM];
	os_net_thread_t pid[WIFI_MODE_TYPE_NUM];
	void * thread_args[WIFI_MODE_TYPE_NUM];
	dispatch_event_t wpad_dispatch_event[WIFI_MODE_TYPE_NUM];
	void *private_data[WIFI_MODE_TYPE_NUM];
	uint8_t wpad_bitmap;
	/*
	 * This bitmap is used to indicate whether the mode is independent;
	 * bit[0]: Are there 2 wpa_supplicant? 1 yes, 0 no;
	 *        If there are 2 wpa_supplicant, sta and p2p are independent
	 * bit[1]: Auxiliary bit, used to indicate whether sta and p2p are independent, only works if bit 1 is 0;
	 * bit[2]: Are wpa_supplicant and hostapd independent and can they coexist? 1 yes, 0 no;
	 * */
	uint8_t wpad_indep_bitmap;
	char wmg_config_path[CONFIG_PATH_MAX];
	char wpa_sta_iface[IFACE_MAX];
	char wpa_sta_config_file[CONFIG_PATH_MAX];
	char wpa_sta_iface_path[CONFIG_PATH_MAX];
	char wpa_p2p_process[PROCESS_MAX];
	char wpa_p2p_iface[IFACE_MAX];
	char wpa_p2p_config_file[CONFIG_PATH_MAX];
	char wpa_p2p_iface_path[CONFIG_PATH_MAX];
	char hapd_ap_iface[IFACE_MAX];
	char hapd_config_file[CONFIG_PATH_MAX];
	char hapd_ap_iface_path[CONFIG_PATH_MAX];
	char wpa_iface_path[CONFIG_PATH_MAX];
	char hapd_iface_path[CONFIG_PATH_MAX];
} wpad_object_t;

static wpad_object_t wpad_object;

const char* get_hapd_ap_iface(void)
{
	return wpad_object.hapd_ap_iface;
}

wmg_status_t stop_linux_process(char* process)
{
	char cmd[128] = {0};
	cmd[127] = '\0';
	int try_cnt = 0;

	sprintf(cmd, "pidof %s | xargs kill -9", process);

	for(; check_process_is_exist(process, strlen(process)); try_cnt++) {
		if(try_cnt < TRY_TIMES) {
			wmg_system(cmd, NULL, 0);
			sleep(1);
		} else {
			return WMG_STATUS_FAIL;
		}
	}

	return WMG_STATUS_SUCCESS;
}

//some process start with para, if process takes a para, use process_para, otherwise use process
wmg_status_t start_linux_process(char* process, char* process_para)
{
	char cmd[256] = {0};
	int try_cnt = 0;

	if(process_para != NULL) {
		sprintf(cmd, "%s", process_para);
	} else {
		sprintf(cmd, "%s", process);
	}

	for(; !check_process_is_exist(process, strlen(process)); try_cnt++) {
		if(try_cnt < TRY_TIMES) {
			wmg_system(cmd, NULL, 0);
			sleep(1);
		} else {
			return WMG_STATUS_FAIL;
		}
	}

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t start_wpad(wifi_mode_t wifi_mode)
{
	char process[18] = {0};
	char cmd[CMD_MAX_LEN * 7] = {0};
	char debug_flag[10] = {0};
	char p2p_iface_opt[CMD_MAX_LEN] = {0}; /* empty string by default; filled with -I option only when SUPPORT_P2P_MODE is enabled */

	if(wmg_get_debug_level() < WMG_MSG_EXCESSIVE) {
		sprintf(debug_flag, "-B");
	} else {
		if(wifi_mode == WIFI_AP) {
			sprintf(debug_flag, "-dd -B");
		} else {
			if(wmg_get_debug_level() == WMG_MSG_EXCESSIVE) {
				sprintf(debug_flag, "-d &");
			} else if(wmg_get_debug_level() == WMG_MSG_EXCESSIVE_MID) {
				sprintf(debug_flag, "-dd &");
			} else if(wmg_get_debug_level() == WMG_MSG_EXCESSIVE_MAX) {
				sprintf(debug_flag, "-ddd &");
			}
		}
	}

	update_wpad_indep_bitmap(&wpad_object.wpad_indep_bitmap);

	if(wifi_mode == WIFI_AP) {
		if(!((wpad_object.wpad_indep_bitmap >> 2) & 0x1)) {
			WMG_DEBUG("wpa_supplicant and hostapd cannot exist at the same time, need to close wpa_supplicant fist\n");
			if(stop_linux_process("wpa_supplicant")) {
				WMG_ERROR("stop wpa_supplicant fail\n");
				return WMG_STATUS_FAIL;
			}
		} else {
			WMG_DEBUG("wpa_supplicant and hostapd can exist at the same time, add a new virtual interface for ap mode\n");
			if(!strcmp(wpad_object.hapd_ap_iface, wpad_object.wpa_sta_iface)) {
				WMG_WARNG("coexistence mode, hapd_ap_iface and wpa_sta_iface have the same name, change hapd_ap_iface to wlan1\n");
				memcpy(wpad_object.hapd_ap_iface, "wlan1", 6);

				/* update interface to hostapd.conf */
				sprintf(cmd, "sed -i \'/^ *interface*/c\\interface=%s\' %s%s"
				, wpad_object.hapd_ap_iface
				, wpad_object.wmg_config_path, wpad_object.hapd_config_file);
				wmg_system(cmd, NULL, 0);
			}
			sprintf(cmd, "iw phy phy0 interface add %s type __ap", wpad_object.hapd_ap_iface);
			wmg_system(cmd, NULL, 0);
		}
		sprintf(process, "hostapd");
		sprintf(cmd, "hostapd %s %s%s", debug_flag, wpad_object.wmg_config_path, wpad_object.hapd_config_file);
	} else {
		if(!((wpad_object.wpad_indep_bitmap >> 2) & 0x1)) {
			WMG_DEBUG("wpa_supplicant and hostapd cannot exist at the same time, need to close hostapd fist\n");
			if(stop_linux_process("hostapd")) {
				WMG_ERROR("stop hostapd fail\n");
				return WMG_STATUS_FAIL;
			}
		}
		sprintf(process, "wpa_supplicant");
		//2 wpa_supplicant
		if(wpad_object.wpad_indep_bitmap & 0x1) {
			if(wifi_mode == WIFI_STATION) {
				sprintf(cmd, "wpa_supplicant -i%s -Dnl80211 -c %s%s -O %s%s %s"
					, wpad_object.wpa_sta_iface
					, wpad_object.wmg_config_path, wpad_object.wpa_sta_config_file
					, wpad_object.wmg_config_path, wpad_object.wpa_sta_iface_path
					, debug_flag);
			} else if (wifi_mode == WIFI_P2P) {
				sprintf(cmd, "p2p_supplicant -i%s -Dnl80211 -c %s%s -O %s%s %s"
					, wpad_object.wpa_p2p_iface
					, wpad_object.wmg_config_path, wpad_object.wpa_p2p_config_file
					, wpad_object.wmg_config_path, wpad_object.wpa_p2p_iface_path
					, debug_flag);
			}
		//1 wpa_supplicant
		} else {
#ifdef SUPPORT_P2P_MODE
			sprintf(p2p_iface_opt, " -I %s%s"
				, wpad_object.wmg_config_path, wpad_object.wpa_p2p_config_file);
#endif
			sprintf(cmd, "wpa_supplicant -i%s -Dnl80211 -c %s%s%s -O %s%s %s"
				, wpad_object.wpa_sta_iface
				, wpad_object.wmg_config_path, wpad_object.wpa_sta_config_file
				, p2p_iface_opt
				, wpad_object.wmg_config_path, wpad_object.wpa_sta_iface_path
				, debug_flag);
		}
	}

	if(start_linux_process(process, cmd)){
		WMG_ERROR("start %s fail\n", cmd);
		return WMG_STATUS_FAIL;
	}

	if(wifi_mode == WIFI_STATION) {
		wpad_object.wpad_bitmap |= WPAD_STA_BITMAP;
	} else if(wifi_mode == WIFI_P2P) {
		wpad_object.wpad_bitmap |= WPAD_P2P_BITMAP;
	} else if(wifi_mode == WIFI_AP) {
		wpad_object.wpad_bitmap |= WPAD_AP_BITMAP;
	}

	return WMG_STATUS_SUCCESS;
}

static wmg_status_t stop_wpad(wifi_mode_t wifi_mode)
{
	if(wifi_mode == WIFI_AP) {
		if(stop_linux_process("hostapd")) {
			WMG_ERROR("stop hostapd fail\n");
			return WMG_STATUS_FAIL;
		}
		wmg_system("ifconfig wlan0 down", NULL, 0);
	} else {
		//2 wpa_supplicant
		if(wpad_object.wpad_indep_bitmap & 0x1) {
			if(wifi_mode == WIFI_STATION) {
				if(stop_linux_process("wpa_supplicant")) {
					WMG_ERROR("stop wpa_supplicant fail\n");
					return WMG_STATUS_FAIL;
				}
			} else if (wifi_mode == WIFI_P2P) {
				if(stop_linux_process("p2p_supplicant")) {
					WMG_ERROR("stop p2p_supplicant fail\n");
					return WMG_STATUS_FAIL;
				}
			}
		//1 wpa_supplicant
		} else {
			uint8_t close_supplicant_flag = 0;
			if(wifi_mode == WIFI_STATION) {
				close_supplicant_flag = wpad_object.wpad_bitmap & (~WPAD_STA_BITMAP);
			} else if(wifi_mode == WIFI_P2P) {
				close_supplicant_flag = wpad_object.wpad_bitmap & (~WPAD_P2P_BITMAP);
			}
			if(close_supplicant_flag & 0x3) {
				WMG_DEBUG("other mode use supplicant now, need not to stop supplicant\n");
				return WMG_STATUS_SUCCESS;
			} else {
				if(stop_linux_process("wpa_supplicant")) {
					WMG_ERROR("stop wpa_supplicant fail\n");
					return WMG_STATUS_FAIL;
				}
			}
		}
	}

	if(wifi_mode == WIFI_STATION) {
		wpad_object.wpad_bitmap &= (~WPAD_STA_BITMAP);
	} else if(wifi_mode == WIFI_P2P) {
		wpad_object.wpad_bitmap &= (~WPAD_P2P_BITMAP);
	} else if(wifi_mode == WIFI_AP) {
		wpad_object.wpad_bitmap &= (~WPAD_AP_BITMAP);
	}

	return WMG_STATUS_SUCCESS;
}

static int wifi_ctrl_recv(char *reply, size_t *reply_len, struct wpa_ctrl *monitor_conn, wifi_mode_t wifi_mode)
{
	int res;
	int ctrlfd = wpa_ctrl_get_fd(monitor_conn);
	struct pollfd rfds;

	memset(&rfds, 0, sizeof(struct pollfd));
	rfds.fd = ctrlfd;
	rfds.events |= POLLIN;
	pthread_testcancel();
	res = TEMP_FAILURE_RETRY(poll(&rfds, 1, -1));
	pthread_testcancel();

	WMG_DUMP("%s:poll = %d, ctrlfd = %d, id = %lu\n",wifi_mode_to_char(wifi_mode), res, ctrlfd, pthread_self());

	if (res < 0) {
		WMG_ERROR("Error poll = %d\n", res);
		return res;
	}
	if (rfds.revents & POLLIN) {
		res = wpa_ctrl_pending(monitor_conn);
		if(res == 1){
			return wpa_ctrl_recv(monitor_conn, reply, reply_len);
		}
	}

	return res;
}

static const char IFNAME[]              = "IFNAME=";
#define IFNAMELEN (sizeof(IFNAME) - 1)
static const char WPA_EVENT_IGNORE[]    = "CTRL-EVENT-IGNORE ";

static int wifi_wait_on_socket(char *buf, size_t buflen, struct wpa_ctrl *monitor_conn, wifi_mode_t wifi_mode)
{
	size_t nread = buflen - 1;
	int result;
	char *match, *match2;

	if (monitor_conn == NULL) {
		return snprintf(buf, buflen, WPA_EVENT_TERMINATING " - connection closed");
	}

	result = wifi_ctrl_recv(buf, &nread, monitor_conn, wifi_mode);

	if (result < 0) {
		WMG_ERROR("wifi_ctrl_recv(%s) failed(result:%d): %s\n",wifi_mode_to_char(wifi_mode), result, strerror(errno));
		return snprintf(buf, buflen, WPA_EVENT_TERMINATING " - recv error");
	}
	WMG_DUMP("wifi_ctrl_recv(%s): %s\n",wifi_mode_to_char(wifi_mode), buf);
	buf[nread] = '\0';
	/* Check for EOF on the socket */
	if (result == 0 && nread == 0) {
		/* Fabricate an event to pass up */
		WMG_EXCESSIVE("Received EOF on supplicant socket\n");
		return snprintf(buf, buflen, WPA_EVENT_TERMINATING " - signal 0 received");
	}
	/*
	 * Events strings are in the format
	 *
	 *     IFNAME=iface <N>CTRL-EVENT-XXX
	 *        or
	 *     <N>CTRL-EVENT-XXX
	 *
	 * where N is the message level in numerical form (0=VERBOSE, 1=Excessive,
	 * etc.) and XXX is the event nae. The level information is not useful
	 * to us, so strip it off.
	 */
	if (strncmp(buf, IFNAME, IFNAMELEN) == 0) {
		match = strchr(buf, ' ');
		if (match != NULL) {
			if (match[1] == '<') {
				match2 = strchr(match + 2, '>');
				if (match2 != NULL) {
					nread -= (match2 - match);
					memmove(match + 1, match2 + 1, nread - (match - buf) + 1);
				}
			}
		} else {
			return snprintf(buf, buflen, "%s", WPA_EVENT_IGNORE);
		}
	} else if (buf[0] == '<') {
		match = strchr(buf, '>');
		if (match != NULL) {
			nread -= (match + 1 - buf);
			memmove(buf, match + 1, nread + 1);
			//printf("supplicant generated event without interface - %s\n", buf);
		}
	} else {
		/* let the event go as is! */
		//printf("supplicant generated event without interface and without message level - %s\n", buf);
	}

	return nread;
}


static void *wpas_event_thread(void *args)
{
	char buf[EVENT_BUF_SIZE] = {0};
	int size, ret;
	wifi_mode_t wifi_mode;
	wifi_mode = *(wifi_mode_t *)args;

	WMG_DEBUG("create wpas event thread wifi mode %s\n",wifi_mode_to_char(wifi_mode));

	for (;;) {
		size = wifi_wait_on_socket(buf, sizeof(buf), wpad_object.monitor_conn[wifi_mode], wifi_mode);
		if (size > 0) {
			dispatch_event_t dispatch = wpad_object.wpad_dispatch_event[wifi_mode];
			/* The event producer can race with an init/deinit that has
			 * not published (or has cleared) the handler. Never call
			 * through a NULL callback (WR-011). */
			if (dispatch == NULL) {
				WMG_WARNG("no dispatch handler for mode %s, drop event\n",
					  wifi_mode_to_char(wifi_mode));
				continue;
			}
			ret = dispatch(buf, size);
			if (ret) {
				WMG_DUMP("wpa_supplicant terminated\n");
				break;
			}
		} else {
			continue;
		}
	}

	WMG_INFO("event thread(wifi_mode:%s) exit now\n",wifi_mode_to_char(wifi_mode));
	pthread_exit(NULL);
}

#define SUPPLICANT_TIMEOUT      3000000  // microseconds
#define SUPPLICANT_TIMEOUT_STEP  100000  // microseconds
static wmg_status_t wifi_connect_on_socket(wifi_mode_t wifi_mode)
{
	int  supplicant_timeout = SUPPLICANT_TIMEOUT;
	int ret_tmp;
	/* socket_err can be reached before any explicit assignment (e.g. the
	 * thread_args malloc failure), so it must not return an indeterminate
	 * value that init_wpad() would mistake for success. */
	wmg_status_t ret = WMG_STATUS_FAIL;
	char path[CONFIG_PATH_MAX * 3] = {0};

	//strncpy(primary_iface, "wlan0", IFACE_VALUE_MAX);
	if (wifi_mode == WIFI_STATION) {
		snprintf(path, sizeof(path), "%s%s/%s", wpad_object.wmg_config_path, wpad_object.wpa_iface_path, wpad_object.wpa_sta_iface);
	} else if (wifi_mode == WIFI_P2P) {
		snprintf(path, sizeof(path), "%s%s/%s", wpad_object.wmg_config_path, wpad_object.wpa_iface_path, wpad_object.wpa_p2p_iface);
	} else if (wifi_mode == WIFI_AP) {
		snprintf(path, sizeof(path), "%s%s/%s", wpad_object.wmg_config_path, wpad_object.hapd_iface_path, wpad_object.hapd_ap_iface);
	}

	wpad_object.ctrl_conn[wifi_mode] = wpa_ctrl_open(path);
	while (wpad_object.ctrl_conn[wifi_mode] == NULL && supplicant_timeout > 0){
		usleep(SUPPLICANT_TIMEOUT_STEP);
		supplicant_timeout -= SUPPLICANT_TIMEOUT_STEP;
		wpad_object.ctrl_conn[wifi_mode] = wpa_ctrl_open(path);
	}
	if (wpad_object.ctrl_conn[wifi_mode] == NULL) {
		WMG_ERROR("Unable to open connection to wpad on \"%s\": %s\n",
			path, strerror(errno));
		return WMG_STATUS_FAIL;
	} else {
		WMG_DEBUG("open connection to wpad on \"%s\"\n", path);
	}

	wpad_object.monitor_conn[wifi_mode] = wpa_ctrl_open(path);
	if (wpad_object.monitor_conn[wifi_mode] == NULL) {
		WMG_ERROR("monitor_conn is NULL!\n");
		wpa_ctrl_close(wpad_object.ctrl_conn[wifi_mode]);
		wpad_object.ctrl_conn[wifi_mode] = NULL;
		return WMG_STATUS_FAIL;
	} else {
		WMG_DEBUG("open connection to wpad on \"%s\"\n", path);
	}

	if (wpa_ctrl_attach(wpad_object.monitor_conn[wifi_mode]) != 0) {
		WMG_ERROR("attach monitor_conn on \"%s\": %s\n",
			path, strerror(errno));
		WMG_ERROR("attach monitor_conn error!\n");
		wpa_ctrl_close(wpad_object.monitor_conn[wifi_mode]);
		wpa_ctrl_close(wpad_object.ctrl_conn[wifi_mode]);
		wpad_object.ctrl_conn[wifi_mode] = wpad_object.monitor_conn[wifi_mode] = NULL;
		return WMG_STATUS_FAIL;
	} else {
		WMG_DEBUG("attach monitor_conn on \"%s\"\n",path);
	}

	if((wpad_object.thread_args[wifi_mode] = malloc(sizeof(wifi_mode_t)))) {
		*(int *)wpad_object.thread_args[wifi_mode] = wifi_mode;
	} else {
		goto socket_err;
	}

	ret_tmp = os_net_thread_create(&wpad_object.pid[wifi_mode], wifi_mode_to_char(wifi_mode), wpas_event_thread,
			wpad_object.thread_args[wifi_mode], 0, 4096);
	if (ret_tmp) {
		WMG_ERROR("failed to create linux event(wifi_mode:%s) handle thread\n",wifi_mode_to_char(wifi_mode));
		free(wpad_object.thread_args[wifi_mode]);
		wpad_object.thread_args[wifi_mode] = NULL;
		ret = WMG_STATUS_FAIL;
		goto socket_err;
	}
	WMG_DEBUG("create linux event(wifi_mode%s) handle thread success\n", wifi_mode_to_char(wifi_mode));

	WMG_DEBUG("connect to wpa_supplicant ok!\n");
	return WMG_STATUS_SUCCESS;

socket_err:
		wpa_ctrl_close(wpad_object.monitor_conn[wifi_mode]);
		wpa_ctrl_close(wpad_object.ctrl_conn[wifi_mode]);
		wpad_object.ctrl_conn[wifi_mode] = wpad_object.monitor_conn[wifi_mode] = NULL;

	return ret;
}

static void wifi_close_sockets(wifi_mode_t wifi_mode)
{
	if (wpad_object.pid[wifi_mode] != 0) {
		os_net_thread_delete(&wpad_object.pid[wifi_mode]);
		wpad_object.pid[wifi_mode] = 0;
		WMG_DEBUG("thread delete wifi_mode(%s)\n", wifi_mode_to_char(wifi_mode));
	}

	if(wpad_object.thread_args[wifi_mode]) {
		free(wpad_object.thread_args[wifi_mode]);
		wpad_object.thread_args[wifi_mode] = NULL;
	}

	if (wpad_object.monitor_conn[wifi_mode] != NULL) {
		wpa_ctrl_detach(wpad_object.monitor_conn[wifi_mode]);
		wpa_ctrl_close(wpad_object.monitor_conn[wifi_mode]);
		wpad_object.monitor_conn[wifi_mode] = NULL;
	}

	if (wpad_object.ctrl_conn[wifi_mode] != NULL) {
		wpa_ctrl_close(wpad_object.ctrl_conn[wifi_mode]);
		wpad_object.ctrl_conn[wifi_mode] = NULL;
	}
}

static void update_wpad_config(void)
{
	char cmd[CMD_MAX_LEN * 5] = {0};

	memcpy(wpad_object.wmg_config_path, WMG_CONFIG_PATH, strlen(WMG_CONFIG_PATH));
	get_config("wpa_sta_iface", wpad_object.wpa_sta_iface, DEFAULT_WPA_STA_IFACE);
	get_config("wpa_sta_config_file", wpad_object.wpa_sta_config_file, DEFAULT_WPA_STA_CONFIG_FILE);
	get_config("wpa_sta_iface_path", wpad_object.wpa_sta_iface_path, DEFAULT_WPA_STA_IFACE_PATH);
	get_config("wpa_p2p_process", wpad_object.wpa_p2p_process, DEFAULT_WPA_P2P_PROCESS);
	get_config("wpa_p2p_iface", wpad_object.wpa_p2p_iface, DEFAULT_WPA_P2P_IFACE);
	get_config("wpa_p2p_config_file", wpad_object.wpa_p2p_config_file, DEFAULT_WPA_P2P_CONFIG_FILE);
	get_config("wpa_p2p_iface_path", wpad_object.wpa_p2p_iface_path, DEFAULT_WPA_P2P_IFACE_PATH);
	get_config("hapd_ap_iface", wpad_object.hapd_ap_iface, DEFAULT_HAPD_AP_IFACE);
	get_config("hapd_config_file", wpad_object.hapd_config_file, DEFAULT_HAPD_CONFIG_FILE);
	get_config("hapd_ap_iface_path", wpad_object.hapd_ap_iface_path, DEFAULT_HAPD_AP_IFACE_PATH);
	get_config("wpa_iface_path", wpad_object.wpa_iface_path, DEFAULT_WPA_IFACE_PATH);
	get_config("hapd_iface_path", wpad_object.hapd_iface_path, DEFAULT_HAPD_IFACE_PATH);

	/* update ctrl_interface to hostapd.conf */
	sprintf(cmd, "sed -i \'/^ *ctrl_interface*/c\\ctrl_interface=%s%s\' %s%s"
			, wpad_object.wmg_config_path, wpad_object.hapd_iface_path
			, wpad_object.wmg_config_path, wpad_object.hapd_config_file);
	wmg_system(cmd, NULL, 0);
	/* update interface to hostapd.conf */
	sprintf(cmd, "sed -i \'/^ *interface*/c\\interface=%s\' %s%s"
			, wpad_object.hapd_ap_iface
			, wpad_object.wmg_config_path, wpad_object.hapd_config_file);
	wmg_system(cmd, NULL, 0);
	/* update interface to dnsmasq.conf (P2P mode only; softap now uses udhcpd) */
	sprintf(cmd, "if [ -f %s/dnsmasq.conf ]; then sed -i '/^ *interface=wlan*/c\\interface=%s' %s/dnsmasq.conf; fi"
			, wpad_object.wmg_config_path
			, wpad_object.hapd_ap_iface
			, wpad_object.wmg_config_path);
	wmg_system(cmd, NULL, 0);
}

wmg_status_t init_wpad(init_wpad_para_t wpad_para)
{
	if(wpad_object.update_wpad_config_flag != true) {
		update_wpad_config();
		wpad_object.update_wpad_config_flag = true;
	}

	/* Publish the dispatch callback and its private data before
	 * wifi_connect_on_socket() attaches the monitor and starts the event
	 * thread: an event can arrive as soon as the thread is created
	 * (WR-011). Withdraw the publication on any failure. */
	wpad_object.wpad_dispatch_event[wpad_para.wifi_mode] = wpad_para.dispatch_event;
	wpad_object.private_data[wpad_para.wifi_mode] = wpad_para.linux_mode_private_data;

	if(start_wpad(wpad_para.wifi_mode)){
		goto error;
	}

	if(wifi_connect_on_socket(wpad_para.wifi_mode))
	{
		stop_wpad(wpad_para.wifi_mode);
		goto error;
	}

	return WMG_STATUS_SUCCESS;

error:
	wpad_object.wpad_dispatch_event[wpad_para.wifi_mode] = NULL;
	wpad_object.private_data[wpad_para.wifi_mode] = NULL;
	return WMG_STATUS_FAIL;
}

wmg_status_t deinit_wpad(wifi_mode_t wifi_mode)
{
	wifi_close_sockets(wifi_mode);
	stop_wpad(wifi_mode);
	wpad_object.wpad_dispatch_event[wifi_mode] = NULL;
	wpad_object.private_data[wifi_mode] = NULL;

	memset(wpad_object.wmg_config_path, 0, CONFIG_PATH_MAX);
	memset(wpad_object.wpa_sta_iface, 0, IFACE_MAX);
	memset(wpad_object.wpa_sta_config_file, 0, CONFIG_PATH_MAX);
	memset(wpad_object.wpa_sta_iface_path, 0, CONFIG_PATH_MAX);
	memset(wpad_object.wpa_p2p_process, 0, PROCESS_MAX);
	memset(wpad_object.wpa_p2p_iface, 0, IFACE_MAX);
	memset(wpad_object.wpa_p2p_config_file, 0, CONFIG_PATH_MAX);
	memset(wpad_object.wpa_p2p_iface_path, 0, CONFIG_PATH_MAX);
	memset(wpad_object.hapd_ap_iface, 0, IFACE_MAX);
	memset(wpad_object.hapd_config_file, 0, CONFIG_PATH_MAX);
	memset(wpad_object.hapd_ap_iface_path, 0, CONFIG_PATH_MAX);
	memset(wpad_object.wpa_iface_path, 0, CONFIG_PATH_MAX);
	memset(wpad_object.hapd_iface_path, 0, CONFIG_PATH_MAX);
	wpad_object.update_wpad_config_flag = false;

	return WMG_STATUS_SUCCESS;
}

static int wifi_send_command(const char *cmd, char *reply, size_t *reply_len, wifi_mode_t wifi_mode, char *p2p_gp_inf)
{
	int ret;
	char path[CONFIG_PATH_MAX * 3] = {0};
	struct wpa_ctrl *p2p_gp_ctrl_conn = NULL;

	if (wpad_object.ctrl_conn[wifi_mode] == NULL) {
		WMG_ERROR("Not connected to wpa_supplicant - \"%s\" command dropped.\n", cmd);
		return -1;
	}

	/* new p2p interface(go/gc), need open new interface */
	if ((wifi_mode == WIFI_P2P) && p2p_gp_inf != NULL) {
		snprintf(path, sizeof(path), "%s%s/%s", wpad_object.wmg_config_path, wpad_object.wpa_iface_path, p2p_gp_inf);
		p2p_gp_ctrl_conn = wpa_ctrl_open(path);
		if (p2p_gp_ctrl_conn == NULL) {
			WMG_ERROR("Connected to %s fail\n", path);
			return -1;
		}
		ret = wpa_ctrl_request(p2p_gp_ctrl_conn, cmd, strlen(cmd), reply, reply_len, NULL);
	} else {
		ret = wpa_ctrl_request(wpad_object.ctrl_conn[wifi_mode], cmd, strlen(cmd), reply, reply_len, NULL);
	}

	if (ret != 0) {
		goto error;
	}

	/* wpa_ctrl_request() returns the reply length but does not append a
	 * trailing NUL. command_to_wpad() reserves one byte for it, so this
	 * is in bounds. Terminate before doing any string operation. */
	reply[*reply_len] = '\0';

	/* A zero return only means "a reply was received". wpa_supplicant /
	 * hostapd report rejected commands as "FAIL", "FAIL-BUSY", ... in the
	 * reply body. Keep the text so callers can distinguish them, but never
	 * report a protocol-level failure as success (WR-012). */
	if (strncmp(reply, "FAIL", 4) == 0) {
		WMG_ERROR("'%s' command rejected by wpad: %s\n", cmd, reply);
		if (p2p_gp_ctrl_conn != NULL) {
			wpa_ctrl_close(p2p_gp_ctrl_conn);
		}
		return -1;
	}

	return 0;

error:
	if (p2p_gp_ctrl_conn != NULL) {
		wpa_ctrl_close(p2p_gp_ctrl_conn);
	}
	if (ret == -2) {
		WMG_ERROR("'%s' command timed out.\n", cmd);
		return -2;
	}
	/* ret < 0: transport/attach failure; reply is not valid here. */
	return -1;
}

/* p2p_gp_inf only for p2p module, other module input NULL */
wmg_status_t command_to_wpad(char const *cmd, char *reply, size_t reply_len, wifi_mode_t wifi_mode, char *p2p_gp_inf)
{
	pthread_mutex_lock(&wpad_object.wpa_supplicant_mutex);
	if(!cmd || !cmd[0]){
		pthread_mutex_unlock(&wpad_object.wpa_supplicant_mutex);
		return WMG_STATUS_FAIL;
	}

	WMG_DUMP("do cmd(wifi_mode:%s) %s\n", wifi_mode_to_char(wifi_mode), cmd);

	--reply_len; // Ensure we have room to add NUL termination.
	if (wifi_send_command(cmd, reply, &reply_len, wifi_mode, p2p_gp_inf) != 0) {
		pthread_mutex_unlock(&wpad_object.wpa_supplicant_mutex);
		return WMG_STATUS_FAIL;
	}

	WMG_DUMP("do cmd(wifi_mode:%s) %s, reply: %s\n", wifi_mode_to_char(wifi_mode), cmd, reply);
	// Strip off trailing newline.
	if (reply_len > 0 && reply[reply_len-1] == '\n') {
		reply[reply_len-1] = '\0';
	} else {
		reply[reply_len] = '\0';
	}
	pthread_mutex_unlock(&wpad_object.wpa_supplicant_mutex);
	return WMG_STATUS_SUCCESS;
}

wmg_status_t module_name_register(char* module_name)
{
	int node_num = 0;

	get_config("wifi_module_name", wpad_object.wifi_module_name, "UNKNOWN");
	memcpy(module_name, wpad_object.wifi_module_name, MODULE_NAME_MAX_LEN);

	node_num = find_match_support_list(module_name);

	if(node_num != -1) {
		strcat(module_name, " (M)");
		return WMG_STATUS_SUCCESS;
	} else {
		strcat(module_name, " (D)");
		return WMG_STATUS_FAIL;
	}
}

static wpad_object_t wpad_object = {
	.wifi_module_name = {0},
	.update_wpad_config_flag = false,
	.wpa_supplicant_mutex = PTHREAD_MUTEX_INITIALIZER,
	.ctrl_conn = {NULL,NULL},
	.monitor_conn = {NULL,NULL},
	.pid = {0, 0},
	.thread_args = {NULL,NULL},
	.wpad_dispatch_event = {NULL,NULL},
	.private_data = {NULL,NULL},
	.wpad_bitmap = 0x0,
	.wpad_indep_bitmap = 0x0,
	.wmg_config_path = {0},
	.wpa_sta_iface = {0},
	.wpa_sta_config_file = {0},
	.wpa_sta_iface_path = {0},
	.wpa_p2p_process = {0},
	.wpa_p2p_iface = {0},
	.wpa_p2p_config_file = {0},
	.wpa_p2p_iface_path = {0},
	.hapd_ap_iface = {0},
	.hapd_config_file = {0},
	.hapd_ap_iface_path = {0},
	.wpa_iface_path = {0},
	.hapd_iface_path = {0},
};
