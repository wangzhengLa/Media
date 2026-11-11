#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <wifimg.h>
#include <wifi_log.h>
#include <linkd.h>
#include <wmg_common.h>

static wmg_linkd_object_t wmg_linkd_object;
static volatile int g_linkd_cancel_requested = 0;

int wmg_linkd_is_cancelled(void)
{
	return g_linkd_cancel_requested;
}

typedef struct {
	proto_main_loop main_loop;
	char name[10];
}main_loop_function_t;

/*
 * we need thread_num to synchronize thread data
 * 1.There may be multiple threads calling the callback function at the same time.
 * 2.Only the data of the current valid thread is necessary
 * 3.The data of non-current threads is invalid, but there is a situation where non-current threads call back this function
 * 4.thread_num is used to determine whether the current thread can modify the data and then pass it to the upper layer.
 */
static void linkd_mode_result_cb(wifi_linkd_result_t *linkd_result, pthread_t thread_num)
{
	int ssid_len, psk_len;

	if(thread_num == wmg_linkd_object.thread) {
		if(linkd_result == NULL){
			WMG_ERROR("get ssid and psk fail\n");
			wmg_linkd_object.result_state = WMG_LINKD_RESULT_FAIL;
			return;
		}

		ssid_len = strlen(linkd_result->ssid);
		psk_len = strlen(linkd_result->psk);
		if(((ssid_len < 0) || (ssid_len > SSID_MAX_LEN)) || ((psk_len < 0) || (psk_len > PSK_MAX_LEN))) {
			WMG_ERROR("get ssid and psk size larger than buff size\n");
			wmg_linkd_object.result_state = WMG_LINKD_RESULT_FAIL;
		} else {
			memset(wmg_linkd_object.ssid_result, '\0', sizeof(wmg_linkd_object.ssid_result));
			memset(wmg_linkd_object.psk_result, '\0', sizeof(wmg_linkd_object.psk_result));
			memcpy(wmg_linkd_object.ssid_result, linkd_result->ssid, ssid_len);
			memcpy(wmg_linkd_object.psk_result, linkd_result->psk, psk_len);
			wmg_linkd_object.result_state = WMG_LINKD_RESULT_SUCCESS;
			WMG_INFO("get ssid(%s) and psk(%s) success(%lu)\n", wmg_linkd_object.ssid_result,
					wmg_linkd_object.psk_result, thread_num);
		}
	} else {
		WMG_WARNG("thread(%lu) is diffrent now_thread, do not strcpy ssid and psk\n", thread_num,
				wmg_linkd_object.thread);
	}
}

void *__attribute__((weak)) _ble_mode_main_loop(void *arg)
{
	WMG_WARNG("wk: unspport ble config net\n");
	proto_main_loop_para_t *main_loop_para = (proto_main_loop_para_t *)arg;
	main_loop_para->result_cb(NULL, 0);
	return NULL;
}

void *__attribute__((weak)) _softap_mode_main_loop(void *arg)
{
	WMG_WARNG("wk: unsupport softap config net\n");
	proto_main_loop_para_t *main_loop_para = (proto_main_loop_para_t *)arg;
	main_loop_para->result_cb(NULL, 0);
	return NULL;
}

main_loop_function_t main_loop_function[WMG_LINKD_PROTO_MAX] = {
	{_ble_mode_main_loop, "ble"},
	{_softap_mode_main_loop, "softap"},
};

static wmg_status_t wmg_linkd_init(int wait_second, void *params)
{
	g_linkd_cancel_requested = 0;
	wmg_linkd_object.linkd_state = WMG_LINKD_IDEL;
	wmg_linkd_object.main_loop_para.result_cb = linkd_mode_result_cb;
	if(wait_second == 0) {
		wait_second = DEFAULT_SECOND;
	} else if(wait_second < 20 || wait_second > 600) {
		wait_second = 20;
	}
	wmg_linkd_object.main_loop_para.wait_second = (wait_second - 20);
	if(params != NULL) {
		wmg_linkd_object.main_loop_para.private = params;
	}
	WMG_INFO("linkd thread set wait_second: %d\n", wmg_linkd_object.main_loop_para.wait_second);
	return WMG_STATUS_SUCCESS;
}

static wmg_status_t wmg_linkd_protocol_enable(wifi_linkd_mode_t mode, void *proto_function_param)
{
	int main_loop_function_num = (int)mode;

	if(wmg_linkd_object.linkd_state != WMG_LINKD_IDEL) {
		WMG_ERROR("Wmg linkd busy, running %d mode now\n",wmg_linkd_object.linkd_mode_state);
		return WMG_STATUS_BUSY;
	}

	WMG_INFO("%s protocol linkd mode\n", main_loop_function[main_loop_function_num].name);

	if(!os_net_thread_create(&wmg_linkd_object.thread, main_loop_function[main_loop_function_num].name, main_loop_function[main_loop_function_num].main_loop, proto_function_param, 20, 4096)){
		WMG_DEBUG("create %s mode main loop pthread success\n", main_loop_function[main_loop_function_num].name);
		wmg_linkd_object.linkd_mode_state = mode;
		/* Mark the slot as taken so a concurrent direct call cannot
		 * overwrite the thread handle before deinit joins it. */
		wmg_linkd_object.linkd_state = WMG_LINKD_RUNNING;
		return WMG_STATUS_SUCCESS;
	} else {
		WMG_ERROR("create %s mode main loop pthread fail\n", main_loop_function[main_loop_function_num].name);
		wmg_linkd_object.linkd_state = WMG_LINKD_IDEL;
		wmg_linkd_object.linkd_mode_state = WMG_LINKD_MODE_NONE;
		return WMG_STATUS_FAIL;
	}
}

static wmg_status_t wmg_linkd_protocol_get_results(wifi_linkd_result_t *linkd_result, int second)
{
	wmg_status_t ret = WMG_STATUS_FAIL;
	int wait_second = DEFAULT_SECOND + 5;
	if(second > 0) {
		wait_second = second + 5;
	}
	while((wait_second > 0) && (wmg_linkd_object.result_state == WMG_LINKD_RESULT_INVALIN)) {
		sleep(1);
		wait_second--;
	}
	/* The loop exits with wait_second == 0, so the result state must be used
	 * to distinguish a timeout from a completed transaction. */
	if(wmg_linkd_object.result_state == WMG_LINKD_RESULT_INVALIN) {
		linkd_result->ssid = NULL;
		linkd_result->psk = NULL;
		WMG_ERROR("linkd protocol get results time out\n");
		ret = WMG_STATUS_TIMEOUT;
	} else if (wmg_linkd_object.result_state == WMG_LINKD_RESULT_SUCCESS) {
		memcpy(linkd_result->ssid, wmg_linkd_object.ssid_result, strlen(wmg_linkd_object.ssid_result));
		memcpy(linkd_result->psk, wmg_linkd_object.psk_result, strlen(wmg_linkd_object.psk_result));
		ret  = WMG_STATUS_SUCCESS;
	} else {
		linkd_result->ssid = NULL;
		linkd_result->psk = NULL;
		WMG_ERROR("linkd protocol can't get results\n");
		ret = WMG_STATUS_FAIL;
	}
	memset(wmg_linkd_object.ssid_result, 0, SSID_MAX_LEN);
	memset(wmg_linkd_object.psk_result, 0, PSK_MAX_LEN);
	wmg_linkd_object.result_state = WMG_LINKD_RESULT_INVALIN;

	return ret;
}

static void wmg_linkd_deinit(void)
{
	/* Cooperative stop: both protocol loops poll the cancel flag and exit
	 * on their own, so in-flight BT actions and their cleanup handlers
	 * always run to completion before we join. No pthread_cancel. */
	g_linkd_cancel_requested = 1;
	if (wmg_linkd_object.thread != 0) {
		pthread_join(wmg_linkd_object.thread, NULL);
	}
	wmg_linkd_object.thread = 0;
	wmg_linkd_object.linkd_mode_state = WMG_LINKD_MODE_NONE;
	memset(wmg_linkd_object.ssid_result, 0, SSID_MAX_LEN);
	memset(wmg_linkd_object.psk_result, 0, PSK_MAX_LEN);
	wmg_linkd_object.result_state = WMG_LINKD_RESULT_INVALIN;
	wmg_linkd_object.linkd_state = WMG_LINKD_OFF;
}

static wmg_linkd_object_t wmg_linkd_object = {
	.ssid_result = {0},
	.psk_result = {0},
	.result_state = WMG_LINKD_RESULT_INVALIN,
	.thread = 0,
	.linkd_state = WMG_LINKD_OFF,
	.linkd_mode_state = WMG_LINKD_MODE_NONE,
	.linkd_init = wmg_linkd_init,
	.linkd_protocol_enable = wmg_linkd_protocol_enable,
	.linkd_protocol_get_results = wmg_linkd_protocol_get_results,
	.linkd_deinit = wmg_linkd_deinit,
};

wmg_status_t wmg_linkd_protocol(wifi_linkd_mode_t mode, void *params, int second, wifi_linkd_result_t *linkd_result)
{
	wmg_status_t ret = WMG_STATUS_FAIL;

	if(wmg_linkd_object.linkd_state == WMG_LINKD_OFF) {
		WMG_DEBUG("Wmg link isn't init, init now\n");
		wmg_linkd_object.linkd_init(second, params);
	}

	if(mode >= WMG_LINKD_MODE_NONE) {
		WMG_INFO("Unsupport protocol linkd mode\n");
		return WMG_STATUS_UNSUPPORTED;
	}

	if(!wmg_linkd_object.linkd_protocol_enable(mode, (void *)&wmg_linkd_object.main_loop_para)) {
		ret = wmg_linkd_object.linkd_protocol_get_results(linkd_result, wmg_linkd_object.main_loop_para.wait_second);
		wmg_linkd_object.linkd_deinit();
		return ret;
	}

	/* enable() did not take ownership. If another session is running, never
	 * join its thread or clear its state from this caller. */
	if (wmg_linkd_object.linkd_state == WMG_LINKD_RUNNING) {
		return WMG_STATUS_BUSY;
	}
	wmg_linkd_object.linkd_deinit();
	return WMG_STATUS_FAIL;
}

void wmg_linkd_cancel(void)
{
	WMG_WARNG("linkd cancel requested\n");
	/* Both the core wait-loop and the protocol loops observe this flag. */
	g_linkd_cancel_requested = 1;
	wmg_linkd_object.result_state = WMG_LINKD_RESULT_FAIL;
	WMG_WARNG("linkd cancel requested: result_state set to FAIL, core wait-loop and protocol loops should observe this and exit\n");
}
