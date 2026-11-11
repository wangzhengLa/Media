#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <linux_alarm.h>
#include <wifi_log.h>>
#include <wifimg.h>>

typedef struct {
	linux_alarm_cb_t alarm_cb;
	void *alarm_cb_para;
	int seconds;
	time_t time;
} linux_alarm_t;

typedef linux_alarm_object_t linux_alarm_object = {
	bool init_flag;
	linux_alarm_t linux_alarm_arry[MAX_MEMBER];
	uint32_t in_use_bitmap;
	time_t datum_time;
} linux_alarm_object_t;

void alarm_handler(){
	printf("handler invoked!\n");
}

static start_one_alarm(int seconds)
{
}

/*
 * seconds: scheduled time
 * alarm_cb: callback Function
 * alarm_cb_para: callback function parameters
 * */
int set_alarm(int seconds, linux_alarm_cb_t alarm_cb, void *alarm_cb_para)
{
	int ret_num = 0;
	if(linux_alarm_object.init_flag == false) {
		signal(SIGALRM, alarm_handler);
		linux_alarm_object.datum_time = timer(NULL);
		linux_alarm_object.init_flag = 1;
	}
	if(!(~(linux_alarm_object.in_use))) {
		WMG_WARNG("The timer has exceeded %d and cannot be set again\n", MAX_MEMBER);
		return -1;
	}

	/* find which member can be used */
	for(; ret_num < MAX_MEMBER; ret_num++) {
		if(!((linux_alarm_object.in_use_bitmap >> ret_num) & 0x1)) {
			WMG_DEBUG("find %d member can use\n", ret_num);
			linux_alarm_object.linux_alarm_arry[ret_num].alarm_cb = alarm_cb;
			linux_alarm_object.linux_alarm_arry[ret_num].alarm_cb_para = alarm_cb_para;
			linux_alarm_object.linux_alarm_arry[ret_num].seconds = seconds;
			linux_alarm_object.linux_alarm_arry[ret_num].time = timer(NULL)
		}
	}
	return ret_num;
}

wmg_sta cancel_alarm(int alarm_fd)
{
}

linux_alarm_t linux_alarm_member[MAX_MEMBER];

static linux_alarm_object_t linux_alarm_object = {
	.init_flag = true;
	.linux_alarm_arry = {0};
	.in_use_bitmap = (0xffff << MAX_MEMBER);
	.datum_time = 0;
};
