#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "event.h"
#include "scan.h"
#include <linux_sta.h>
#include <linux_common.h>
#include "wifi_log.h"
#include "utils.h"
#include "wmg_sta.h"

#define WAITING_CLK_COUNTS   50
#define SSID_LEN	512
#define TRY_SAN_MAX 6

unsigned char convert_decimal(char str)
{
	unsigned char str_nu = 0;
	if(str >= '0' && str <='9') {
		str_nu = str - '0';
	} else if (str >= 'a' && str <='f') {
		str_nu = str - 'a' + 10;
	} else if (str >= 'A' && str <='F') {
		str_nu = str - 'A' + 10;
	} else {
		str_nu = 0;
	}
	return str_nu;
}

/*
 * ret: returns the converted length
 */
int convert_utf8(char *str, char *buf)
{
	int i = 0, j = 0;
	while(str[i] != '\0') {
		if(((str[i] == '\\') && (str[i + 1] == 'x')) || ((str[i] == '\\') && (str[i+1] == 'X'))) {
			buf[j] = (convert_decimal(str[i + 2]) << 4) + convert_decimal(str[i + 3]);
			i += 3;
		} else {
			buf[j] = str[i];
		}
		j++;
		i++;
	}
	buf[j] = '\0';
	return strlen(buf);
}

/**
 * Format of scan results are following:
 * bssid / frequency / signal level / flags / ssid
 * 00:1c:a3:14:6a:de       2462    -32     [WPA2-PSK-CCMP][ESS]    AW-PD4-R818
 * 50:d2:f5:f1:b7:08       2412    -56     [WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][WPS][ESS]       AW-PDC-PD2-fly2.4g
 */
int parse_scan_results(char *recv_results, wifi_scan_result_t *scan_results,
		uint32_t *bss_num, uint32_t arr_size)
{
	int j = 0, len = 0, res_len = 0;
	char *pos = NULL, *pre = NULL;
	char tmp[128] = {0};
	char *pch = NULL;
	char *delim = "]";
	char *key_mgmt_p = NULL;
	/* The Chinese characters in wpa_supplicant are 4 times the ascii code */
	char utf8_buf[SSID_MAX_LEN + 1] = {0};
	char utf8_tmp_buf[SSID_MAX_LEN * 4] = {0};
	int utf8_ssid_len = 0;
	int utf8_tmp_buf_len = sizeof(utf8_tmp_buf);

	if (recv_results == NULL || scan_results == NULL ||
		bss_num == NULL || arr_size == 0) {
		WMG_ERROR("invalid parameters\n");
		return -1;
	} else {
		*bss_num = 0;
		WMG_DEBUG("scan arr size buff len: %d\n", arr_size);
	}

	res_len = strlen(recv_results);
	WMG_EXCESSIVE("scan results length is %d\n", res_len);
	pos = strchr(recv_results, '\n');
	if (pos == NULL) {
		WMG_WARNG("scan results is NULL\n");
		return -1;
	}
	pos++;  /* pointer to the first bss info */
	while((pos != NULL) && (*pos != '\0')) {
		pre = pos;
		pos = strchr(pos, '\t');  /* pointer to the tab at the end of bssid */
		if (pos == NULL)
			break;
		len = ((pos - pre) > utf8_tmp_buf_len) ? utf8_tmp_buf_len : (pos - pre);  /* calculate the length of bssid */
		if(*bss_num < arr_size) {
			memset(tmp, '\0', sizeof(tmp));
			strncpy(tmp, pre, len);
			WMG_EXCESSIVE("bssid=%s\n", tmp);
			pch = strtok(tmp, ":");
			for(j = 0;((pch != NULL) && (j < 6)); j++) {
				scan_results->bssid[j] = char2uint8(pch);
				pch = strtok(NULL, ":");
			}
		}

		pos++;  /* pointer to frequency */
		pre = pos;
		pos = strchr(pos, '\t');
		if (pos == NULL)
			break;
		len = ((pos - pre) > utf8_tmp_buf_len) ? utf8_tmp_buf_len : (pos - pre);
		memset(tmp, '\0', sizeof(tmp));
		strncpy(tmp, pre, len);
		if(*bss_num < arr_size) {
			scan_results->freq = atoi(tmp);
			WMG_EXCESSIVE("freq=%d\n", scan_results->freq);
		}

		memset(tmp, '\0', sizeof(tmp));
		pos++;  /* pointer to signal level */
		pre = pos;
		pos = strchr(pos, '\t');
		if (pos == NULL)
			break;
		len = ((pos - pre) > utf8_tmp_buf_len) ? utf8_tmp_buf_len : (pos - pre);
		strncpy(tmp, pre, len);
		if(*bss_num < arr_size) {
			scan_results->rssi = atoi(tmp);
			WMG_EXCESSIVE("rssi=%d\n", scan_results->rssi);
		}

		pos++;  /* pointer to encryption flags */
		pre = pos;
		pos = strchr(pos, '\t');
		if (pos == NULL)
			break;
		len = ((pos - pre) > utf8_tmp_buf_len) ? utf8_tmp_buf_len : (pos - pre);
		memset(tmp, '\0', sizeof(tmp));
		strncpy(tmp, pre, len);
		if(*bss_num < arr_size) {
			key_mgmt_p = strtok(tmp, delim);
			scan_results->key_mgmt = WIFI_SEC_NONE;
			while(key_mgmt_p != NULL) {
				if(strncmp(key_mgmt_p, "[WEP", 4) == 0) {
					scan_results->key_mgmt |= WIFI_SEC_WEP;
				}
				if(strncmp(key_mgmt_p, "[WPA-", 5) == 0) {
					if(strstr(key_mgmt_p, "EAP")) {
						scan_results->key_mgmt |= WIFI_SEC_EAP;
					} else {
						scan_results->key_mgmt |= WIFI_SEC_WPA_PSK;
					}
				}
				if(strncmp(key_mgmt_p, "[WPA2", 5) == 0) {
					if(strstr(key_mgmt_p, "SAE")) {
						scan_results->key_mgmt |= WIFI_SEC_WPA3_PSK;
					}
					if(strstr(key_mgmt_p, "WPA2-PSK-SHA256")) {
						scan_results->key_mgmt |= WIFI_SEC_WPA2_PSK_SHA256;
					} else if(strstr(key_mgmt_p, "WPA2-PSK")) {
						scan_results->key_mgmt |= WIFI_SEC_WPA2_PSK;
					} else if(strstr(key_mgmt_p, "EAP")) {
						scan_results->key_mgmt |= WIFI_SEC_EAP;
					}
				}
				if(strncmp(key_mgmt_p, "[SAE", 4) == 0) {
					scan_results->key_mgmt |= WIFI_SEC_WPA3_PSK;
				}
				key_mgmt_p = strtok(NULL, delim);
			}
			WMG_EXCESSIVE("key_mgmt=%d\n", scan_results->key_mgmt);
		}

		pos++;  /* pointer to ssid */
		pre = pos;
		pos = strchr(pos, '\n');
		if (pos == NULL)
			break;
		len = ((pos - pre) > utf8_tmp_buf_len) ? 0 : (pos - pre);
		pos++;  /* advance the parser even when the output array is full */
		if(*bss_num < arr_size) {
			memset(utf8_buf, '\0', sizeof(utf8_buf));
			memset(utf8_tmp_buf, '\0', utf8_tmp_buf_len);
			memset(scan_results->ssid, '\0', sizeof(scan_results->ssid));
			if(len != 0) {
				strncpy(utf8_tmp_buf, pre, len);
				utf8_ssid_len = convert_utf8(utf8_tmp_buf, utf8_buf);
				if(utf8_ssid_len <= SSID_MAX_LEN) {
					memcpy(scan_results->ssid, utf8_buf, utf8_ssid_len);
				} else {
					utf8_ssid_len = 0;
				}
			}
			if(utf8_ssid_len) {
				WMG_DUMP("ssid=%s len=%d\n", scan_results->ssid, utf8_ssid_len);
			} else {
				WMG_WARNG("scan ssid too long, we set to NULL\n");
			}
			scan_results++;  /* store next bss info */
			(*bss_num)++;  /* count only entries actually stored */
		}
	}

	return 0;
}

void remove_slash_from_scan_results(char *recv_results)
{
	char *ptr = NULL;
	char *ptr_s = NULL;
	char *ftr = NULL;

	ptr_s = recv_results;
	while(1) {
		ptr = strchr(ptr_s,'\"');
		if (ptr == NULL)
			break;

		ptr_s = ptr;
		if ((*(ptr - 1)) == '\\') {
			ftr = ptr;
			ptr -= 1;
			while (*ftr != '\0')
				*(ptr++) = *(ftr++);
			*ptr = '\0';
			continue; //restart new search at ptr_s after removing slash
		} else
			ptr_s++; //restart new search at ptr_s++
    }

	ptr_s = recv_results;
	while(1) {
		ptr = strchr(ptr_s,'\\');
		if (ptr == NULL)
			break;

		ptr_s = ptr;
		if((*(ptr - 1)) == '\\') {
			ftr = ptr;
			ptr -= 1;
			while (*ftr != '\0')
				*(ptr++) = *(ftr++);
			*ptr = '\0';
			continue; //restart new search at ptr_s after removing slash
		} else
			ptr_s++; //restart new search at ptr_s++
	}

	//return 0;
}
