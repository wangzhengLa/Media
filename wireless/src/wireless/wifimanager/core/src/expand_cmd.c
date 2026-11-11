#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <wifimg.h>
#include <wifi_log.h>
#include <expand_cmd.h>

wmg_status_t __attribute__((weak)) wmg_linux_send_expand_cmd(char *expand_cmd, void *expand_params, void *expand_cb)
{
	WMG_WARNG("wk: unspport linux exp cmd\n");
	return WMG_STATUS_UNSUPPORTED;
}

wmg_status_t wmg_send_expand_cmd(char *expand_cmd, void *expand_params, void *expand_cb)
{
	if(!strncmp(expand_cmd, "linux:", 6)) {
		return wmg_linux_send_expand_cmd((expand_cmd + 6), expand_params, expand_cb);
	} else {
		WMG_ERROR("unspport os expand_cmd: %s\n", expand_cmd);
		return WMG_STATUS_UNSUPPORTED;
	}
}
