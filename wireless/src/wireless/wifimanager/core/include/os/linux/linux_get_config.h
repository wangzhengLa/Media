#ifndef __LINUX_GET_CONFIG_H__
#define __LINUX_GET_CONFIG_H__

#include "wmg_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WMG_CONFIG_PATH
#define WMG_CONFIG_PATH "/etc/wifi"
#endif

wmg_status_t get_config(char *config_name, char *config_buf, char *config_default);
wmg_status_t set_config(char *config_name, char *config_buf);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /*  __LINUX_GET_CONFIG_H__ */
