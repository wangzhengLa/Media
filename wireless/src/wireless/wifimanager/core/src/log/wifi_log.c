#include <syslog.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdbool.h>
#include <wifi_log.h>
#include <unistd.h>

#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <zlog.h>

typedef struct {
	wmg_log_level_t wmg_debug_level;
	int wmg_log_para;
} global_log_type_t;

static global_log_type_t global_log = {
	.wmg_debug_level = WMG_MSG_INFO,
	.wmg_log_para = 0,
};

static void wmg_printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

static int syslog_priority(int level)
{
	switch (level) {
	case WMG_MSG_EXCESSIVE:
	case WMG_MSG_MSGDUMP:
	case WMG_MSG_DEBUG:
		return LOG_DEBUG;
	case WMG_MSG_INFO:
		return LOG_NOTICE;
	case WMG_MSG_WARNING:
		return LOG_WARNING;
	case WMG_MSG_ERROR:
		return LOG_ERR;
	}
	return LOG_INFO;
}

static zlog_category_t *wmg_zlog_handle = NULL;

#define ZLOG_CONDATA_PATH "/etc/conf/wmg_log.conf"

static int wmg_level_to_zlog(int level)
{
	switch (level) {
	case WMG_MSG_ERROR:
		return ZLOG_LEVEL_ERROR;
	case WMG_MSG_WARNING:
		return ZLOG_LEVEL_WARN;
	case WMG_MSG_MSGDUMP:
	case WMG_MSG_DEBUG:
	case WMG_MSG_EXCESSIVE:
	case WMG_MSG_EXCESSIVE_MID:
	case WMG_MSG_EXCESSIVE_MAX:
		return ZLOG_LEVEL_DEBUG;
	default:
		return ZLOG_LEVEL_INFO;
	}
}

static void wmg_zlog_file(int level, const char *file, const char *func, int line, const char *fmt, va_list ap)
{
	va_list ap2;

	if (wmg_zlog_handle == NULL)
		return;

	va_copy(ap2, ap);
	vzlog(wmg_zlog_handle,
		file ? file : "", file ? (int)strlen(file) : 0,
		func ? func : "", func ? (int)strlen(func) : 0,
		line, wmg_level_to_zlog(level), fmt, ap2);
	va_end(ap2);
}

int wmg_log_init(void)
{
	int rc = zlog_init(ZLOG_CONDATA_PATH);

	if (rc != 0) {
		WMG_ERROR("zlog_init fail(%s)\n", ZLOG_CONDATA_PATH);
		return -1;
	}

	wmg_zlog_handle = zlog_get_category("wmg");
	if (wmg_zlog_handle == NULL) {
		WMG_ERROR("zlog_get_category(wmg) fail\n");
		zlog_fini();
		return -1;
	}
	return 0;
}

void wmg_log_deinit(void)
{
	if (wmg_zlog_handle != NULL) {
		wmg_zlog_handle = NULL;
		zlog_fini();
	}
}

void wmg_print(int level, int para_enable, const char *tag, const char *file, const char *func, int line, const char *fmt, ...)
{
	va_list ap, ap2, ap3;
	va_start(ap, fmt);

	if(level <= global_log.wmg_debug_level) {
		va_copy(ap2, ap);
		vsyslog(syslog_priority(level), fmt, ap2);
		va_end(ap2);

		if(para_enable && (global_log.wmg_log_para != 0)) {
			char strtimestamp[64];
			struct timeval tv;
			struct tm nowtm;
			gettimeofday(&tv, NULL);
			localtime_r(&tv.tv_sec, &nowtm);
			strftime(strtimestamp, sizeof(strtimestamp), "%Y-%m-%d %H:%M:%S", &nowtm);
			wmg_printf("%s:%03d: ", strtimestamp, tv.tv_usec / 1000);
		}

		if(tag != NULL) {
			wmg_printf("%s", tag);
		}

		if(para_enable && (global_log.wmg_log_para != 0)) {
			if(file != NULL) {
				wmg_printf("[%s:%s:%d]", file, func, line);
			}
		}

		va_copy(ap2, ap);
		vprintf(fmt, ap2); //main output
		va_end(ap2);
		fflush(stdout);

		va_copy(ap3, ap);
		wmg_zlog_file(level, file, func, line, fmt, ap3);
		va_end(ap3);
	}

	va_end(ap);
	return;
}

void wmg_set_debug_level(wmg_log_level_t level)
{
	global_log.wmg_debug_level = level;
}

int wmg_get_debug_level()
{
	return global_log.wmg_debug_level;
}

void wmg_set_log_para(int log_para)
{
	global_log.wmg_log_para = log_para;
}

int wmg_get_log_para()
{
	return global_log.wmg_log_para;
}

/*
 * This interface is used to transfer standard output to other io devices
 * @fd(> 0): witch io devices fd
 * @fd(-1): printing switches back to standard output
 * */
void wmg_set_log_fd(int fd)
{
	static int old_stdout = -1;
	fflush(stdout);
	usleep(5000);
	if(fd == -1) {
		if(old_stdout != -1) {
			dup2(old_stdout, 1);
			close(old_stdout);
			old_stdout = -1;
		}
	} else {
		if(old_stdout == -1) {
			old_stdout = dup(1);
			dup2(fd, 1);
		}
	}
}