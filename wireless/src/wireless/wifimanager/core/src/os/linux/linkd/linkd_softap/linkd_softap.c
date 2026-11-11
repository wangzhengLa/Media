#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <wifimg.h>
#include <wifi_log.h>
#include <linkd.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include "scan.h"

#define PORT_DEFAULT 8066

struct soft_ap_resource {
	int fd;
	/* +1 keeps room for the terminating NUL of a full-length SSID/PSK */
	char ssid[SSID_MAX_LEN + 1];
	char psk[PSK_MAX_LEN + 1];
};

/* Cleanup wrapper for pthread_cleanup_push: closes the UDP socket fd.
 * Matches the BLE pattern: pthread_cleanup_pop(1) runs this on every
 * exit path, releasing port 8066 before the next softap start.
 *
 * Cancellation is cooperative, so recvfrom() returns normally and the
 * handler always runs to completion. */
static void _softap_fd_cleanup(void *arg)
{
	int *fdp = (int *)arg;
	if (fdp && *fdp >= 0) {
		close(*fdp);
		*fdp = -1;
	}
}

/* Bounded parser for the SoftAP credential datagram.
 * Layout: "::SSID::"<ssid>"::Password::"<psk>"::End::"
 * The caller must pass a NUL-terminated buffer. Each marker is searched
 * only after the previous one, so pointer differences are non-negative and
 * no field is copied before its length is validated (WR-009). */
static int _softap_parse_credential(char *buff, char *ssid, size_t ssid_size,
				    char *psk, size_t psk_size)
{
	const char ssid_tag[] = "::SSID::";
	const char psk_tag[]  = "::Password::";
	const char end_tag[]  = "::End::";
	char *ssid_start, *psk_pos, *psk_start, *end_pos;
	size_t ssid_len, psk_len;

	ssid_start = strstr(buff, ssid_tag);
	if (ssid_start == NULL)
		return -1;
	ssid_start += sizeof(ssid_tag) - 1;

	psk_pos = strstr(ssid_start, psk_tag);
	if (psk_pos == NULL)
		return -1;
	psk_start = psk_pos + sizeof(psk_tag) - 1;

	end_pos = strstr(psk_start, end_tag);
	if (end_pos == NULL)
		return -1;

	/* The searches above guarantee psk_pos >= ssid_start and
	 * end_pos >= psk_start, so both lengths are non-negative. */
	ssid_len = (size_t)(psk_pos - ssid_start);
	psk_len = (size_t)(end_pos - psk_start);
	if (ssid_len == 0 || psk_len == 0 ||
	    ssid_len >= ssid_size || psk_len >= psk_size)
		return -1;

	memcpy(ssid, ssid_start, ssid_len);
	ssid[ssid_len] = '\0';
	memcpy(psk, psk_start, psk_len);
	psk[psk_len] = '\0';
	return 0;
}

void *_softap_mode_main_loop(void *arg)
{
	/* NO pthread_detach(pthread_self()) — linkd_deinit needs pthread_join
	 * to synchronize with this thread's exit before clearing wmg_linkd_object.
	 * See linkd_ble.c for the identical fix on the BLE path. */
	WMG_INFO("support softap mode config net(%lu)\n", pthread_self());
	proto_main_loop_para_t *main_loop_para = (proto_main_loop_para_t *)arg;
	int wait_second = main_loop_para->wait_second;

	wifi_linkd_result_t linkd_result;

	struct soft_ap_resource resource = {
		.fd = -1,
		.ssid = {0},
		.psk = {0},
	};

	/* Push cleanup handler right after resource init.
	 * resource.fd starts at -1 so _softap_fd_cleanup is a no-op
	 * until socket() succeeds and sets it to a real fd. */
	pthread_cleanup_push(_softap_fd_cleanup, &resource.fd);

	int port = PORT_DEFAULT;

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	int sock;
	if((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0){
		WMG_ERROR("*****socket error!*****\n");
		WMG_ERROR("*****%s *****\n",strerror(errno));
		main_loop_para->result_cb(NULL, pthread_self());
		goto exit;
	}else{
		WMG_INFO("*****socket success port:%d.*****\n", port);
		resource.fd = sock;
	}

	if(bind(sock, (struct sockaddr *)&addr, sizeof(addr))< 0){
		WMG_ERROR("*****bind error!*****\n");
		WMG_ERROR("*****%s *****\n", strerror(errno));
		main_loop_para->result_cb(NULL, pthread_self());
		goto exit;
	}else{
		WMG_INFO("bind success.\n");
	}

	char buff[512];
	struct sockaddr_in clientAddr;
	int len = sizeof(clientAddr);
	int sock_ret;
	struct timeval tv;

	if(wait_second <= 0) {
		wait_second = DEFAULT_SECOND;
	}
	/* One-second slices let the receive loop observe a cooperative cancel
	 * without pthread_cancel interrupting recvfrom(). */
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		WMG_ERROR("setsockopt failed");
		main_loop_para->result_cb(NULL, pthread_self());
		goto exit;
	}
	WMG_INFO("recv slice 1 second, total %d second\n", wait_second);

	time_t deadline = time(NULL) + wait_second;
	while(1) {
		if (wmg_linkd_is_cancelled()) {
			WMG_WARNG("softap recv cancelled\n");
			main_loop_para->result_cb(NULL, pthread_self());
			goto exit;
		}
		WMG_INFO("recvfrom...\n");
		sock_ret = recvfrom(sock, buff, 511, 0, (struct sockaddr *)&clientAddr, (socklen_t *)&len);
		if(sock_ret > 0){
			/* recvfrom() does not append a NUL. buff is 512 bytes and at
			 * most 511 are read, so buff[sock_ret] is always in bounds.
			 * Terminate before any strstr() on the datagram. */
			buff[sock_ret] = '\0';
			/* The datagram carries the PSK in clear text; never log it. */
			WMG_INFO("*** recvfrom got %d bytes ***\n", sock_ret);
			break;
		} else if(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
			/* Wall-clock deadline, so repeated EINTR cannot shorten the wait */
			if(time(NULL) >= deadline) {
				WMG_WARNG("*** softap recv timeout ***\n");
				main_loop_para->result_cb(NULL, pthread_self());
				goto exit;
			}
			continue;
		} else {
			WMG_WARNG("*** recvfrom failed ***\n");
			main_loop_para->result_cb(NULL, pthread_self());
			goto exit;
		}
	}

	/* Validate the datagram BEFORE acknowledging it. Sending OK first
	 * made the peer believe a malformed/oversized credential was accepted,
	 * while we actually dropped it and reported failure (WR-009/010). */
	if (_softap_parse_credential(buff,
				     resource.ssid, sizeof(resource.ssid),
				     resource.psk, sizeof(resource.psk)) != 0) {
		WMG_ERROR("malformed softap credential datagram, drop it\n");
		main_loop_para->result_cb(NULL, pthread_self());
		goto exit;
	}
	/* Only lengths are logged: SSID/PSK must not leak into the log. */
	WMG_INFO("softap credential accepted (ssid_len=%zu, psk_len=%zu)\n",
		 strlen(resource.ssid), strlen(resource.psk));

	char buff_confirm[3];
	strcpy(buff_confirm,"OK");
	{
		int ack_len = strlen(buff_confirm);
		int ack_retries = 0;
		while(1) {
			if (wmg_linkd_is_cancelled() || ack_retries++ > 50) {
				WMG_WARNG("softap ack %s\n",
					  wmg_linkd_is_cancelled() ? "cancelled" : "timeout");
				main_loop_para->result_cb(NULL, pthread_self());
				goto exit;
			}
			sock_ret = sendto(sock, buff_confirm, ack_len, 0,
					  (struct sockaddr *)&clientAddr, sizeof(clientAddr));
			if(sock_ret > 0){
				WMG_DEBUG("sendto success.\n");
				break;
			}
			/* Only transient conditions are worth retrying. A fatal
			 * socket error would otherwise spin through the whole
			 * budget (~500ms) and delay a cooperative cancel. */
			if (sock_ret < 0 && errno != EINTR && errno != EAGAIN &&
			    errno != EWOULDBLOCK) {
				WMG_WARNG("softap ack failed: %s\n", strerror(errno));
				main_loop_para->result_cb(NULL, pthread_self());
				goto exit;
			}
			usleep(10000);
		}
	}

	linkd_result.ssid = resource.ssid;
	linkd_result.psk = resource.psk;
	main_loop_para->result_cb(&linkd_result, pthread_self());

exit:
	/* Single pop at the unified exit point — runs _softap_fd_cleanup
	 * which closes resource.fd. Every exit path (timeout, cancel, error,
	 * success) lands here so the UDP port is always released. */
	pthread_cleanup_pop(1);
	WMG_INFO("exit softap mode config net(%lu)\n", pthread_self());
	return NULL;
}
