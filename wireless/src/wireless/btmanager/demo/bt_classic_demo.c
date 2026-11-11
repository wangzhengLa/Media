/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: command for test.
*/

#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include "bt_dev_list.h"
#include "bt_log.h"
#include "bt_test.h"
#include "dirent.h"
#include <alsa/asoundlib.h>

#define AVRCP_METADATE_TMP 1

static void cmd_bt_enable(int argc, char *args[])
{
    int ret = BT_OK;
    int value = 0;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    value = atoi(args[0]);
    if (value)
        ret = bt_manager_enable(true);
    else
        ret = bt_manager_enable(false);

    if (ret < 0)
        BTMG_ERROR("error:%s", bt_manager_get_error_info(ret));
}

static void cmd_bt_scan(int argc, char *args[])
{
    int value = 0;

    if (argc < 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    btmg_scan_filter_t scan_filter = { 0 };
    scan_filter.type = BTMG_SCAN_BR_EDR;
    scan_filter.rssi = -90;
    value = atoi(args[0]);

    if (argc == 2) {
        if (strcmp(args[1], "ble") == 0) {
            scan_filter.type = BTMG_SCAN_LE;
        } else if (strcmp(args[1], "br") == 0) {
            scan_filter.type = BTMG_SCAN_BR_EDR;
        }
    }

    if (value) {
        bt_manager_scan_filter(&scan_filter);
        bt_manager_start_scan();
    } else {
        bt_manager_stop_scan();
    }
}

static void cmd_bt_pair(int argc, char *args[])
{
    int ret = BT_OK;

    if (argc != 1 && strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    ret = bt_manager_pair(args[0]);
    if (ret < 0)
        BTMG_ERROR("Pair failed:%s,error:%s", args[0], bt_manager_get_error_info(ret));
}

static void cmd_bt_unpair(int argc, char *args[])
{
    if (argc != 1 && strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    if (bt_manager_unpair(args[0]))
        BTMG_ERROR("Pair failed:%s", args[0]);
}

static void cmd_bt_inquiry_list(int argc, char *args[])
{
    dev_node_t *dev_node = NULL;

    dev_node = discovered_devices->head;
    while (dev_node != NULL) {
        BTMG_INFO("addr: %s, name: %s", dev_node->dev_addr, dev_node->dev_name);
        dev_node = dev_node->next;
    }
}

static void cmd_bt_pair_devices_list(int argc, char *args[])
{
    int i = 0;
    int count = -1;
    btmg_bt_device_t *devices = NULL;

    bt_manager_get_paired_devices(&devices, &count);
    if (count > 0) {
        for (i = 0; i < count; i++) {
            printf("\n \
				address:%s \n \
				name:%s \n \
				paired:%d \n \
				connected:%d\n",
                   devices[i].remote_address, devices[i].remote_name, devices[i].paired,
                   devices[i].connected);
        }
    } else {
        BTMG_ERROR("paired device is empty");
    }
    bt_manager_free_paired_devices(devices, count);
}

static void cmd_bt_get_adapter_state(int argc, char *args[])
{
    btmg_adapter_state_t bt_state;
    bt_state = bt_manager_get_adapter_state();
    if (bt_state == BTMG_ADAPTER_OFF)
        BTMG_INFO("The BT is OFF");
    else if (bt_state == BTMG_ADAPTER_ON)
        BTMG_INFO("The BT is ON");
    else if (bt_state == BTMG_ADAPTER_TURNING_ON)
        BTMG_INFO("The BT is turning ON");
    else if (bt_state == BTMG_ADAPTER_TURNING_OFF)
        BTMG_INFO("The BT is turning OFF");
}

static void cmd_bt_get_adapter_name(int argc, char *args[])
{
    char bt_name[128] = { 0 };

    bt_manager_get_adapter_name(bt_name);

    BTMG_INFO("bt adater name: %s", bt_name);
}

static void cmd_bt_set_adapter_name(int argc, char *args[])
{
    int ret = -1;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    if (strlen(args[0]) > 127) {
        BTMG_ERROR("invalid params, exceed maximum length 127");
        return;
    }

    if (bt_manager_set_adapter_name(args[0])) {
        BTMG_ERROR("Set bt name failed.");
    }
}

static void cmd_bt_get_device_name(int argc, char *args[])
{
    int ret = BT_OK;
    char remote_name[128] = { 0 };

    if (argc != 1 || strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    ret = bt_manager_get_device_name(args[0], remote_name);
    if (ret) {
        BTMG_ERROR("get bt remote name fail.");
    } else {
        BTMG_INFO("get bt remote name:%s", remote_name);
    }
}

static void cmd_bt_get_adapter_address(int argc, char *args[])
{
    int ret = BT_OK;
    char bt_addr[18] = { 0 };

    ret = bt_manager_get_adapter_address(bt_addr);
    if (ret) {
        BTMG_ERROR("get bt address failed.");
    } else {
        BTMG_INFO("BT address: %s", bt_addr);
    }
}

static void cmd_bt_avrcp(int argc, char *args[])
{
    int avrcp_type = -1;
    char bt_addr[18] = { 0 };
    int i;
    int ret;
    const char *cmd_type_str[] = {
        "play", "pause", "stop", "fastforward", "rewind", "forward", "backward",
    };
    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    for (i = 0; i < sizeof(cmd_type_str) / sizeof(char *); i++) {
        if (strcmp(args[0], cmd_type_str[i]) == 0) {
            avrcp_type = i;
            break;
        }
    }

    if (avrcp_type == -1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    ret = bt_manager_get_adapter_address(bt_addr);
    if (ret) {
        BTMG_ERROR("get bt address failed.");
        return;
    } else {
        BTMG_INFO("BT address:%s", bt_addr);
    }

    bt_manager_avrcp_command(bt_addr, (btmg_avrcp_command_t)avrcp_type);
}

static void cmd_bt_avrcp_tg_metadate(int argc, char *args[])
{
    btmg_metadata_info_t metadata = {0};
    char bt_addr[18] = { 0 };
    int ret = BT_OK;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    strcpy(metadata.title, args[0]);
    metadata.duration_ms = 1000;
    strcpy(metadata.genre, "new genre");

    bt_manager_avrcp_send_metadata(metadata);
}

static void cmd_bt_avrcp_set_abs_volume(int argc, char *args[])
{
    int ret = BT_OK;
    uint16_t volume = 0;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    volume = atoi(args[0]);
    if (volume > 127) {
        BTMG_ERROR("volume set range 0~127");
        return;
    }

    ret = bt_manager_avrcp_set_abs_volume(volume);
    if (ret) {
        BTMG_ERROR("avrcp set abs volume fail.");
    } else {
        BTMG_INFO("success set absolut volume:%d", volume);
    }
}

static void cmd_bt_avrcp_get_abs_volume(int argc, char *args[])
{
    int ret = BT_OK;
    uint16_t volume = 0;

    ret = bt_manager_avrcp_get_abs_volume(&volume);
    if (ret) {
        BTMG_ERROR("avrcp get abs volume fail.");
    } else {
        BTMG_INFO("now absolut volume:%d", volume);
    }
}

static void cmd_bt_device_connect(int argc, char *args[])
{
    int ret = BT_OK;

    if (argc != 1 && strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    ret = bt_manager_connect(args[0]);

    if (ret < 0)
        BTMG_ERROR("error:%s", bt_manager_get_error_info(ret));
}

static void cmd_bt_device_disconnect(int argc, char *args[])
{
    int ret = BT_OK;

    if (argc != 1 && strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    ret = bt_manager_disconnect(args[0]);

    if (ret < 0)
        BTMG_ERROR("error:%s", bt_manager_get_error_info(ret));
}

static void cmd_bt_remove_device(int argc, char *args[])
{
    if (argc != 1 && strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    bt_manager_remove_device(args[0]);
}

typedef struct {
    char music_path[128];
    int music_number;
} wav_musiclist_t;

typedef struct {
    char type;
    char parameter[128];
} a2dp_src_run_args_t;

static bool a2dp_src_loop = false;
static bool a2dp_src_enable = false;
static bool a2dp_src_playstate_play = false;
static bool a2dp_src_playstate_pause = false;
static pthread_t bt_test_a2dp_source_thread;
static a2dp_src_run_args_t *a2dp_src_run_args = NULL;
wav_musiclist_t *a2dp_source_musiclist = NULL;
static int a2dp_source_musiclist_number = 1;

#define A2DP_SRC_BUFF_SIZE (1024 * 4)
#define MAX_LRC_LINE_LENGTH 256

typedef struct {
    int minutes;
    int seconds;
    int milliseconds;
    char text[MAX_LRC_LINE_LENGTH];
} lrcline_t;

int parse_lrc_file(const char* fileName, lrcline_t** lrclines, int* numOfLines, btmg_metadata_info_t *metadata) {
    FILE* file = fopen(fileName, "r");
    if (file == NULL) {
        BTMG_WARNG("Failed to open LRC file");
        return -1;
    }

    char line[MAX_LRC_LINE_LENGTH];
    int count = 0;
    while (fgets(line, sizeof(line), file)) {
        int minutes, seconds, milliseconds;

        if (sscanf(line, "[%d:%d.%d]", &minutes, &seconds, &milliseconds) == 3) {
            char* text = strchr(line, ']') + 1;
            if (*text != '\0') {
                lrcline_t* lrcline = (lrcline_t*)malloc(sizeof(lrcline_t));
                lrcline->minutes = minutes;
                lrcline->seconds = seconds;
                lrcline->milliseconds = milliseconds;
                strcpy(lrcline->text, text);
                lrcline->text[strcspn(lrcline->text, "\r\n")] = '\0';
                lrclines[count++] = lrcline;
            }
        } else if (sscanf(line, "[ti:%127[^]]", metadata->title) == 1) {
            BTMG_DEBUG("title: %s", metadata->title);
        } else if (sscanf(line, "[ar:%127[^]]", metadata->artist) == 1) {
            BTMG_DEBUG("artist: %s", metadata->artist);
        } else if (sscanf(line, "[al:%127[^]]", metadata->album) == 1) {
            BTMG_DEBUG("album: %s", metadata->album);
        }
    }

    *numOfLines = count;
    fclose(file);
    return 0;
}

static int _a2dp_source_traverse_musiclist(char *foldpath)
{
    char file_format[8] = { 0 };
    DIR *record_dir = NULL;
    struct dirent *de = NULL;
    FILE *file = NULL;
    int file_count = 0;

    record_dir = opendir(foldpath);
    if (record_dir == NULL) {
        BTMG_ERROR("Path OPEN error");
        return -1;
    }

    if (a2dp_source_musiclist != NULL) {
        free(a2dp_source_musiclist);
    }
    a2dp_source_musiclist_number = 1;
    a2dp_source_musiclist = (wav_musiclist_t *)malloc(1 * sizeof(wav_musiclist_t));

    while ((de = readdir(record_dir)) != 0) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        } else if (de->d_type == 8) { /* file */
            int filelen = strlen(de->d_name);
            memset(file_format, '\0', sizeof(file_format));
            strncpy(file_format, de->d_name + filelen - 3, 3); /* record the file format */
            if (!strcmp("wav", file_format)) {
                wav_musiclist_t *ml = &a2dp_source_musiclist[a2dp_source_musiclist_number - 1];
                if (foldpath[strlen(foldpath) - 1] != '/')
                    sprintf(ml->music_path, "%s/%s", foldpath, de->d_name);
                else
                    sprintf(ml->music_path, "%s%s", foldpath, de->d_name);

                ml->music_number = a2dp_source_musiclist_number;
                a2dp_source_musiclist_number++;
                BTMG_DEBUG("find file path : %s", ml->music_path);

                wav_musiclist_t * new_musiclist;
                new_musiclist = (wav_musiclist_t *)realloc(a2dp_source_musiclist,
                                                                   a2dp_source_musiclist_number *
                                                                           sizeof(wav_musiclist_t));
                if (new_musiclist == NULL) {
                    BTMG_ERROR("realloc fail");
                    closedir(record_dir);
                    return -1;
                }
                else {
                    a2dp_source_musiclist = new_musiclist;
                }
            }
        }
    }
    closedir(record_dir);
    return 0;
}

static inline unsigned int calc_wav_ms(int data_size, int sample_rate, int bit_per_sample, int channel) // Byte, khz, bit, num
{
    // data_size - 44
    return (data_size) * 8 / (sample_rate * bit_per_sample * channel) * 1000;
}

#define check_wavefile_space(buffer, len, blimit) \
    if (len > blimit) { \
        void *temp = realloc(buffer, len); \
        if (temp == NULL) { \
            BTMG_ERROR("not enough memory"); \
            return -1; \
        } \
        buffer = temp; \
        blimit = len; \
    }

static struct {
    snd_pcm_format_t format;
    uint16_t channels;
    uint32_t rate;
    uint32_t data_size;
} wav_params;

/*
 * Safe read (for pipes)
 */

static ssize_t safe_read(int fd, void *buf, size_t count)
{
    ssize_t result = 0, res;

    while (count > 0) {
        if ((res = read(fd, buf, count)) == 0)
            break;
        if (res < 0)
            return result > 0 ? result : res;
        count -= res;
        result += res;
        buf = (char *)buf + res;
    }
    return result;
}

/*
 * helper for test_wavefile
 */

static size_t test_wavefile_read(int fd, u_char *buffer, size_t *size, size_t reqsize, int line)
{
    if (*size >= reqsize)
        return *size;
    if ((size_t)safe_read(fd, buffer + *size, reqsize - *size) != reqsize - *size) {
        BTMG_ERROR("read BTMG_ERROR (called from line %i)", line);
        pthread_exit((void *)-1);
    }
    return *size = reqsize;
}
/*
 * test, if it's a .WAV file, > 0 if ok (and set the speed, stereo etc.)
 *                            == 0 if not
 * Value returned is bytes to be discarded.
 */
static ssize_t test_wavefile(int fd, u_char *_buffer, size_t size)
{
    WaveHeader *h = (WaveHeader *)_buffer;
    u_char *buffer = NULL;
    size_t blimit = 0;
    WaveFmtBody *f;
    WaveChunkHeader *c;
    u_int type, len;
    unsigned short format, channels;
    int big_endian, native_format;

    if (size < sizeof(WaveHeader))
        return -1;
    if (h->magic == WAV_RIFF)
        big_endian = 0;
    else if (h->magic == WAV_RIFX)
        big_endian = 1;
    else
        return -1;
    if (h->type != WAV_WAVE)
        return -1;

    if (size > sizeof(WaveHeader)) {
        check_wavefile_space(buffer, size - sizeof(WaveHeader), blimit);
        memcpy(buffer, _buffer + sizeof(WaveHeader), size - sizeof(WaveHeader));
    }
    size -= sizeof(WaveHeader);
    while (1) {
        check_wavefile_space(buffer, sizeof(WaveChunkHeader), blimit);
        test_wavefile_read(fd, buffer, &size, sizeof(WaveChunkHeader), __LINE__);
        c = (WaveChunkHeader*)buffer;
        if (c == NULL)
            return -1;
        type = c->type;
        len = TO_CPU_INT(c->length, big_endian);
        len += len % 2;
        if (size > sizeof(WaveChunkHeader))
            memmove(buffer, buffer + sizeof(WaveChunkHeader), size - sizeof(WaveChunkHeader));
        size -= sizeof(WaveChunkHeader);
        if (type == WAV_FMT)
            break;
        check_wavefile_space(buffer, len, blimit);
        test_wavefile_read(fd, buffer, &size, len, __LINE__);
        if (size > len)
            memmove(buffer, buffer + len, size - len);
        size -= len;
    }

    if (len < sizeof(WaveFmtBody)) {
        BTMG_ERROR("unknown length of 'fmt ' chunk (read %u, should be %u at least)",
            len, (u_int)sizeof(WaveFmtBody));
        return -1;
    }
    check_wavefile_space(buffer, len, blimit);
    test_wavefile_read(fd, buffer, &size, len, __LINE__);
    f = (WaveFmtBody*) buffer;
    format = TO_CPU_SHORT(f->format, big_endian);
    if (format == WAV_FMT_EXTENSIBLE) {
        WaveFmtExtensibleBody *fe = (WaveFmtExtensibleBody*)buffer;
        if (len < sizeof(WaveFmtExtensibleBody)) {
            BTMG_ERROR("unknown length of extensible 'fmt ' chunk (read %u, should be %u at least)",
                    len, (u_int)sizeof(WaveFmtExtensibleBody));
            return -1;
        }
        if (memcmp(fe->guid_tag, WAV_GUID_TAG, 14) != 0) {
            BTMG_ERROR("wrong format tag in extensible 'fmt ' chunk");
            return -1;
        }
        format = TO_CPU_SHORT(fe->guid_format, big_endian);
    }
    if (format != WAV_FMT_PCM &&
        format != WAV_FMT_IEEE_FLOAT) {
                BTMG_ERROR("can't play WAVE-file format 0x%04x which is not PCM or FLOAT encoded", format);
        return -1;
    }
    channels = TO_CPU_SHORT(f->channels, big_endian);
    if (channels < 1) {
        BTMG_ERROR("can't play WAVE-files with %d tracks", channels);
        return -1;
    }
    wav_params.channels = channels;
    switch (TO_CPU_SHORT(f->bit_p_spl, big_endian)) {
    case 8:
        native_format = SND_PCM_FORMAT_U8;
        break;
    case 16:
        if (big_endian)
            native_format = SND_PCM_FORMAT_S16_BE;
        else
            native_format = SND_PCM_FORMAT_S16_LE;
        break;
    case 24:
        switch (TO_CPU_SHORT(f->byte_p_spl, big_endian) / wav_params.channels) {
        case 3:
            if (big_endian)
                native_format = SND_PCM_FORMAT_S24_3BE;
            else
                native_format = SND_PCM_FORMAT_S24_3LE;
            break;
        case 4:
            if (big_endian)
                native_format = SND_PCM_FORMAT_S24_BE;
            else
                native_format = SND_PCM_FORMAT_S24_LE;
            break;
        default:
            BTMG_ERROR("can't play WAVE-files with sample %d bits in %d bytes wide (%d channels)",
                TO_CPU_SHORT(f->bit_p_spl, big_endian),
                TO_CPU_SHORT(f->byte_p_spl, big_endian),
                wav_params.channels);
            return -1;
        }
        break;
    case 32:
        if (format == WAV_FMT_PCM) {
            if (big_endian)
                native_format = SND_PCM_FORMAT_S32_BE;
            else
                native_format = SND_PCM_FORMAT_S32_LE;
        } else if (format == WAV_FMT_IEEE_FLOAT) {
            if (big_endian)
                native_format = SND_PCM_FORMAT_FLOAT_BE;
            else
                native_format = SND_PCM_FORMAT_FLOAT_LE;
        }
        break;
    default:
        BTMG_ERROR(" can't play WAVE-files with sample %d bits wide",
            TO_CPU_SHORT(f->bit_p_spl, big_endian));
        return -1;
    }
    wav_params.format = native_format;
    wav_params.rate = TO_CPU_INT(f->sample_fq, big_endian);

    if (size > len)
        memmove(buffer, buffer + len, size - len);
    size -= len;

    while (1) {
        u_int type, len;

        check_wavefile_space(buffer, sizeof(WaveChunkHeader), blimit);
        test_wavefile_read(fd, buffer, &size, sizeof(WaveChunkHeader), __LINE__);
        c = (WaveChunkHeader*)buffer;
        if (c == NULL)
            return -1;
        type = c->type;
        len = TO_CPU_INT(c->length, big_endian);
        if (size > sizeof(WaveChunkHeader))
            memmove(buffer, buffer + sizeof(WaveChunkHeader), size - sizeof(WaveChunkHeader));
        size -= sizeof(WaveChunkHeader);
        if (type == WAV_DATA) {
            if (len < 0x7ffffffe)
                wav_params.data_size = len;
            if (size > 0)
                memcpy(_buffer, buffer, size);
            free(buffer);
            return size;
        }
        len += len % 2;
        check_wavefile_space(buffer, len, blimit);
        test_wavefile_read(fd, buffer, &size, len, __LINE__);
        if (size > len)
            memmove(buffer, buffer + len, size - len);
        size -= len;
    }

    /* shouldn't be reached */
    return -1;
}

static void *_a2dp_source_thread_func(void *arg)
{
    char muisc_path[256] = { 0 };
    int path_length = 0;
    int ret = -1, len = 0, fd = -1, fd_lrc;
    char buffer[A2DP_SRC_BUFF_SIZE] = { 0 };
    unsigned int c = 0, written = 0, count = 0;
    a2dp_src_run_args_t *a2dp_source_thread_arg = (a2dp_src_run_args_t *)arg;
    WaveHeader wav_header;
    static int wav_number = 0;
    int bits_per_sample;
    char type = a2dp_source_thread_arg->type;
    int data_offset = 44;

    if (type == 'f') {
        strcpy(muisc_path, a2dp_source_thread_arg->parameter);
    } else if (type == 'p') {
        ret = _a2dp_source_traverse_musiclist(a2dp_source_thread_arg->parameter);
        if (ret == BT_ERROR) {
            BTMG_ERROR("_a2dp_source_thread_func EMD");
            pthread_exit((void *)-1);
        }
        strcpy(muisc_path, (const char *)&a2dp_source_musiclist[wav_number].music_path);
    }
start:
    len = 0, c = 0, written = 0, count = 0;
    memset(buffer, 0, sizeof(buffer));
    path_length = strlen(muisc_path);
    if (path_length < 5) { //File path meets at least length 5
        BTMG_ERROR("Please enter the correct file path");
        a2dp_src_enable = false;
        pthread_exit((void *)-1);
    }
    if (strcmp(".wav", &muisc_path[path_length - 4])) {
        BTMG_ERROR("Please enter the correct audio format - 'wav' ");
        a2dp_src_enable = false;
        pthread_exit((void *)-1);
    }

    fd = open(muisc_path, O_RDONLY); // "/mnt/44100-stereo-s16_le-10s.wav"
    if (fd < 0) {
        BTMG_ERROR("Cannot open input file:%s", strerror(errno));
        a2dp_src_enable = false;
        pthread_exit((void *)-1);
    }
    ret = read(fd, &wav_header, sizeof(WaveHeader));
    if (ret != sizeof(WaveHeader)) {
        BTMG_ERROR("read wav file header failed.");
        close(fd);
        pthread_exit((void *)-1);
    }

    /* read bytes for WAVE-header */
    if (test_wavefile(fd, &wav_header, sizeof(WaveHeader)) < 0) {
        BTMG_ERROR("read wav file header failed.");
        close(fd);
        pthread_exit((void *)-1);
    }

    BTMG_INFO("wav file size:%u, data size:%u, ch:%hu, rate:%u, format:%s", wav_header.length, wav_params.data_size,
        wav_params.channels, wav_params.rate, snd_pcm_format_name(wav_params.format));

    data_offset = wav_header.length + 8 - wav_params.data_size;
    bits_per_sample = snd_pcm_format_physical_width(wav_params.format);

    if (bt_manager_a2dp_src_init(wav_params.channels, wav_params.rate, (int)wav_params.format) != 0) {
        BTMG_ERROR("a2dp source init error");
        close(fd);
        pthread_exit((void *)-1);
    }
#if AVRCP_METADATE_TMP
    btmg_metadata_info_t metadata = {
        .title = "unknow",
        .artist = "unknow",
        .album = "unknow",
        .genre = "unknow",
        .track_num = 1,
        .duration_ms = 0xffff,
    };
    int last_send_postion_ms = 0;
    char lrc_path[256] = { 0 };

    // open lyrics
    strcpy(lrc_path, muisc_path);
    path_length = strlen(muisc_path);
    strcpy(&lrc_path[path_length-3], "lrc");

    lrcline_t* lrcLines[100] = {NULL}; // set max line 100
    int numOfLines = 0;
    int current_line = 0;
    int lrc_ret = parse_lrc_file(lrc_path, lrcLines, &numOfLines, &metadata);
    BTMG_DEBUG("lrc_ret=%d lrc_path=%s numOfLines=%d\n", lrc_ret, lrc_path, numOfLines);

    for (int i = 0; i < numOfLines; i++) {
        printf("[%02d:%02d.%03d] %s\n", lrcLines[i]->minutes, lrcLines[i]->seconds,
               lrcLines[i]->milliseconds, lrcLines[i]->text);
    }

    metadata.duration_ms = calc_wav_ms(wav_params.data_size, wav_params.rate, bits_per_sample, wav_params.channels);
#endif

    ret = -1;
    a2dp_src_enable = true;
    a2dp_src_loop = true;
    count = wav_params.data_size;
    bt_manager_set_avrcp_status(BTMG_AVRCP_PLAYSTATE_PLAYING);
    bt_manager_a2dp_src_stream_start(A2DP_SRC_BUFF_SIZE);
    BTMG_INFO("start a2dp src loop");

    while (a2dp_src_loop) {
        btmg_avrcp_play_state_t status = bt_manager_get_avrcp_status();
        switch (status) {
        case BTMG_AVRCP_PLAYSTATE_PLAYING:
            if (a2dp_src_playstate_pause) {
                ret = -1;
                a2dp_src_playstate_pause = false;
                bt_manager_a2dp_src_stream_start(A2DP_SRC_BUFF_SIZE);
            }
            if (ret != 0) {
                c = count - written;
                if (c > A2DP_SRC_BUFF_SIZE) {
                    c = A2DP_SRC_BUFF_SIZE;
                }
                len = read(fd, buffer, c);
                if (len == 0) {
                    /*offset move to header*/
                    lseek(fd, data_offset, SEEK_SET);
                    written = 0;
#if AVRCP_METADATE_TMP
                    current_line = 0;
                    last_send_postion_ms = 0;
#endif
                    continue;
                }
                if (len < 0) {
                    BTMG_ERROR("read file error,ret:%d,c=%d", len, c);
                    break;
                }
            } else {
                usleep(1000 * 20);
            }
            if (len > 0) {
                ret = bt_manager_a2dp_src_stream_send(buffer, len);
                written += ret;
            }
#if AVRCP_METADATE_TMP
            if (lrc_ret == 0) {
                int cur_time_ms = calc_wav_ms(written, wav_params.rate, bits_per_sample, wav_params.channels);
                if (lrcLines[current_line] != NULL) {
                    int lyric_time_ms = ((lrcLines[current_line]->minutes * 60)
                                        + lrcLines[current_line]->seconds) * 1000
                                        + lrcLines[current_line]->milliseconds;

                    if (cur_time_ms > lyric_time_ms) {
                        BTMG_DEBUG("cur_line=%d lyric_time_ms=%d cur_time_ms=%d", current_line, lyric_time_ms, cur_time_ms);
                        strncpy(metadata.title, lrcLines[current_line]->text, strlen(lrcLines[current_line]->text)+1);
                        bt_manager_avrcp_send_metadata(metadata);
                        current_line++;
                    }
                }
                if ((cur_time_ms - last_send_postion_ms) > 3000) {
                    bt_manager_avrcp_send_position((uint64_t)cur_time_ms * 1000);
                    last_send_postion_ms = cur_time_ms;
                }
            }
#endif
            break;
        case BTMG_AVRCP_PLAYSTATE_PAUSED:
            if (!a2dp_src_playstate_pause) {
                bt_manager_a2dp_src_stream_stop(false);
                a2dp_src_playstate_pause = true;
            }
            usleep(10 * 1000);
            break;

        case BTMG_AVRCP_PLAYSTATE_FORWARD:
            if (!a2dp_src_playstate_pause) {
                bt_manager_a2dp_src_stream_stop(false);
                bt_manager_a2dp_src_deinit();
                a2dp_src_playstate_pause = true;
            }
            if (type == 'p' || type == 'P') {
                wav_number++;
                if (wav_number >= a2dp_source_musiclist_number - 1) {
                    wav_number = 0;
                }
                memset(muisc_path, 0, sizeof(muisc_path));
                strcpy(muisc_path, (const char *)&a2dp_source_musiclist[wav_number].music_path);
            }
            close(fd);
            a2dp_src_loop = false;
            a2dp_src_enable = false;
#if AVRCP_METADATE_TMP
            if (lrc_ret == 0 && numOfLines) {
                for (int i = 0; i < numOfLines; i++) {
                    free(lrcLines[i]);
                    lrcLines[i] = NULL;
                }
            }
#endif
            usleep(500 * 1000);
            goto start;
            break;
        case BTMG_AVRCP_PLAYSTATE_BACKWARD:
            if (!a2dp_src_playstate_pause) {
                bt_manager_a2dp_src_stream_stop(false);
                bt_manager_a2dp_src_deinit();
                a2dp_src_playstate_pause = true;
            }
            if (type == 'p' || type == 'P') {
                wav_number--;
                if (wav_number < 0) {
                    wav_number = a2dp_source_musiclist_number - 1 - 1;
                }
                memset(muisc_path, 0, sizeof(muisc_path));
                strcpy(muisc_path, (const char *)&a2dp_source_musiclist[wav_number].music_path);
            }
            close(fd);
            a2dp_src_loop = false;
            a2dp_src_enable = false;
#if AVRCP_METADATE_TMP
            if (lrc_ret == 0 && numOfLines) {
                for (int i = 0; i < numOfLines; i++) {
                    free(lrcLines[i]);
                    lrcLines[i] = NULL;
                }
            }
#endif
            usleep(500 * 1000);
            goto start;
            break;
        default:
            break;
        }
    }
    if (fd != -1) {
        close(fd);
    }

#if AVRCP_METADATE_TMP
    if (lrc_ret == 0 && numOfLines) {
        for (int i = 0; i < numOfLines; i++) {
            free(lrcLines[i]);
            lrcLines[i] = NULL;
        }
    }
#endif
    bt_manager_a2dp_src_stream_stop(false);
    bt_manager_a2dp_src_deinit();
    a2dp_src_enable = false;

    BTMG_INFO("a2dp source play thread quit");

    return NULL;
}

static void cmd_bt_a2dp_src_run(int argc, char *args[])
{
    if (a2dp_src_enable == true) {
        BTMG_INFO("a2dp source play thread already started");
        return;
    }
    if (a2dp_src_loop == false) {
#if 0
		// int i = 0, opt = 0;
		// const char *optstring = "F:f:P:p:";//fire or path
		// char **a2dp_src_args_list = (char **)malloc( 10 * sizeof(a2dp_src_args_list) );
		// for (i = 0; i < argc; i++){
      	// 	a2dp_src_args_list[i + 1] = args[i];
		// 	BTMG_ERROR("a2dp_src_args_list[%d] = %s",i+1, a2dp_src_args_list[i+1]);
   		// }
		// while ((opt = getopt(argc + 1, a2dp_src_args_list, optstring)) != -1){
        // 	printf("opt = %c\t\t", opt);
       	// 	printf("optarg = %s\t\t",optarg);
        // 	printf("optind = %d\t\t",optind);
        // 	printf("argv[optind] = %s\n",a2dp_src_args_list[optind]);
		// }
#endif
        if (a2dp_src_run_args)
            free(a2dp_src_run_args);
        a2dp_src_run_args = (a2dp_src_run_args_t *)malloc(sizeof(a2dp_src_run_args_t));
        if (strcmp("-f", args[0]) == 0 || strcmp("-F", args[0]) == 0) {
            a2dp_src_run_args->type = 'f';
            memset(a2dp_src_run_args->parameter, 0, sizeof(a2dp_src_run_args->parameter));
            strcpy(a2dp_src_run_args->parameter, args[1]);
        } else if (strcmp("-p", args[0]) == 0 || strcmp("-P", args[0])) {
            a2dp_src_run_args->type = 'p';
            memset(a2dp_src_run_args->parameter, 0, sizeof(a2dp_src_run_args->parameter));
            strcpy(a2dp_src_run_args->parameter, args[1]);
        } else {
            BTMG_ERROR("Please enter the parameters correctly");
            free(a2dp_src_run_args);
            return;
        }
        pthread_create(&bt_test_a2dp_source_thread, NULL, _a2dp_source_thread_func,
                       (void *)a2dp_src_run_args);
    }
}

static void cmd_bt_a2dp_src_set_status(int argc, char *args[])
{
    if (a2dp_src_enable == true) {
        if (strcmp("play", args[0]) == 0) {
            bt_manager_set_avrcp_status(BTMG_AVRCP_PLAYSTATE_PLAYING);
        } else if (strcmp("pause", args[0]) == 0) {
            bt_manager_set_avrcp_status(BTMG_AVRCP_PLAYSTATE_PAUSED);
        } else if (strcmp("forward", args[0]) == 0) {
            bt_manager_set_avrcp_status(BTMG_AVRCP_PLAYSTATE_FORWARD);
        } else if (strcmp("backward", args[0]) == 0) {
            bt_manager_set_avrcp_status(BTMG_AVRCP_PLAYSTATE_BACKWARD);
        }
        return;
    }
    if (a2dp_src_loop == false) {
        BTMG_INFO("a2dp source play thread no start");
    }
}

void bt_a2dp_src_stop(void)
{
    if (a2dp_src_enable == false) {
        BTMG_INFO("a2dp source play thread doesn't run");
        return;
    }

    a2dp_src_loop = false;
    pthread_join(bt_test_a2dp_source_thread, NULL);
    bt_manager_set_avrcp_status(BTMG_AVRCP_PLAYSTATE_STOPPED);
    if (a2dp_src_run_args) {
        free(a2dp_src_run_args);
        a2dp_src_run_args = NULL;
    }

    if (a2dp_source_musiclist) {
        free(a2dp_source_musiclist);
        a2dp_source_musiclist = NULL;
    }
}

static void cmd_bt_a2dp_src_stop(int argc, char *args[])
{
	bt_a2dp_src_stop();
}

bool bt_a2dp_src_is_run(void)
{
    return a2dp_src_loop;
}

static void cmd_bt_a2dp_set_vol(int argc, char *args[])
{
    int vol_value = 0;

    vol_value = atoi(args[0]);
    if (vol_value > 100)
        vol_value = 100;

    if (vol_value < 0)
        vol_value = 0;

    bt_manager_a2dp_set_vol(vol_value);

    BTMG_INFO("a2dp set vol:%d", vol_value);
}

static void cmd_bt_a2dp_get_vol(int argc, char *args[])
{
    int vol_value = 0;

    vol_value = bt_manager_a2dp_get_vol();

    BTMG_INFO("a2dp get vol:%d", vol_value);
}

static void cmd_bt_hfp_answer_call(int argc, char *args[])
{
    bt_manager_hfp_hf_send_at_ata();
}

static void cmd_bt_hfp_hangup(int argc, char *args[])
{
    bt_manager_hfp_hf_send_at_chup();
}

static void cmd_bt_hfp_dial_num(int argc, char *args[])
{
    bt_manager_hfp_hf_send_at_atd(args[0]);
}

static void cmd_bt_hfp_subscriber_number(int argc, char *args[])
{
    bt_manager_hfp_hf_send_at_cnum();
}

static void cmd_bt_hfp_last_num_dial(int argc, char *args[])
{
    bt_manager_hfp_hf_send_at_bldn();
}

static void cmd_bt_hfp_hf_vgs_volume(int argc, char *args[])
{
    int val;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    val = atoi(args[0]);
    if (val > 15 || val < 0) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    bt_manager_hfp_hf_send_at_vgs(val);
}

static void cmd_bt_hfp_send_at_cmd(int argc, char *args[])
{
    int ret = BT_OK;

    if (argc != 1 && strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    ret = bt_manager_hfp_hf_send_at_cmd(args[0]);

    if (ret < 0)
        BTMG_ERROR("hfp ag sco conn fail");
}

static void cmd_bt_hfp_ag_sco_conn(int argc, char *args[])
{
    int ret = BT_OK;

    if (argc != 1 && strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    ret = bt_manager_hfp_ag_sco_conn_cmd(args[0]);

    if (ret < 0)
        BTMG_ERROR("hfp ag sco conn fail");
}

static void cmd_bt_hfp_ag_sco_disconn(int argc, char *args[])
{
    bt_manager_hfp_ag_sco_disconn_cmd();
}

static void cmd_bt_spp_client_connect(int argc, char *args[])
{
    if (argc != 1 && strlen(args[0]) != 17) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    bt_manager_spp_client_connect(args[0]);
}

static void cmd_bt_spp_client_send(int argc, char *args[])
{
    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    bt_manager_spp_client_send(args[0], strlen(args[0]));
}

static void cmd_bt_spp_client_disconnect(int argc, char *args[])
{
    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    bt_manager_spp_client_disconnect(args[0]);
}

static void cmd_bt_spp_service_accept(int argc, char *args[])
{
    bt_manager_spp_service_accept();
}

static void cmd_bt_spp_service_send(int argc, char *args[])
{
    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    bt_manager_spp_service_send(args[0], strlen(args[0]));
}

static void cmd_bt_spp_service_disconnect(int argc, char *args[])
{
    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    bt_manager_spp_service_disconnect(args[0]);
}

static void cmd_bt_set_ex_debug_mask(int argc, char *args[])
{
    int val;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    val = atoi(args[0]);
    bt_manager_set_ex_debug_mask(val);
}

static void cmd_bt_set_scan_mode(int argc, char *args[])
{
    int val;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    val = atoi(args[0]);
    if (val > 2 || val < 0) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    bt_manager_set_scan_mode((btmg_scan_mode_t)val);
}
static void cmd_bt_set_page_timeout(int argc, char *args[])
{
    int val;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    val = atoi(args[0]);
    if (val > 65535 || val < 1) {
        BTMG_ERROR("page timeout out of range: 1~65535");
        return;
    }

    bt_manager_set_page_timeout(val);
}

static void cmd_bt_set_io_capability(int argc, char *args[])
{
    int val;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    val = atoi(args[0]);
    if (val > 4 || val < 0) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    bt_manager_agent_set_io_capability((btmg_io_capability_t)val);
}

cmd_tbl_t bt_cmd_table[] = {
    { "enable", NULL, cmd_bt_enable, "enable [0/1]: open bt or not" },
    { "scan", NULL, cmd_bt_scan, "scan [0/1][br/ble]: scan for devices" },
    { "scan_list", NULL, cmd_bt_inquiry_list, "scan_list: list available devices" },
    { "pair", NULL, cmd_bt_pair, "pair [mac]: pair with devices" },
    { "unpair", NULL, cmd_bt_unpair, "unpair [mac]: unpair with devices" },
    { "paired_list", NULL, cmd_bt_pair_devices_list, "paired_list: list paired devices" },
    { "get_adapter_state", NULL, cmd_bt_get_adapter_state,
      "get_adapter_state: get bt adapter state" },
    { "get_adapter_name", NULL, cmd_bt_get_adapter_name, "get_adapter_name: get bt adapter name" },
    { "set_adapter_name", NULL, cmd_bt_set_adapter_name,
      "set_adapter_name [name]: set bt adapter name" },
    { "get_adapter_addr", NULL, cmd_bt_get_adapter_address,
      "get_adapter_addr: get bt adapter address" },
    { "get_device_name", NULL, cmd_bt_get_device_name,
      "get_device_name[mac]: get remote device name" },
    { "set_scan_mode", NULL, cmd_bt_set_scan_mode,
      "set_scan_mode [0~2]:0-NONE,1-page scan,2-inquiry scan&page scan" },
    { "set_page_to", NULL, cmd_bt_set_page_timeout, "real timeout = slots * 0.625ms" },
    { "set_io_cap", NULL, cmd_bt_set_io_capability,
      "set_io_cap [0~4]:0-keyboarddisplay,1-displayonly,2-displayyesno,3-keyboardonly,4-noinputnooutput" },
    { "avrcp", NULL, cmd_bt_avrcp,
      "avrcp [play/pause/stop/fastforward/rewind/forward/backward]: avrcp control" },
    { "avrcp_send_title", NULL, cmd_bt_avrcp_tg_metadate, "avrcp_send_title [title]: avrcp target send title" },
    { "avrcp_set_abs_volume", NULL, cmd_bt_avrcp_set_abs_volume, "avrcp_set_abs_volume [volume]: as source set absolute volume 0~127" },
    { "avrcp_get_abs_volume", NULL, cmd_bt_avrcp_get_abs_volume, "avrcp_get_abs_volume: as source get absolute volume 0~127" },
    { "connect", NULL, cmd_bt_device_connect, "connect [mac]:generic method to connect" },
    { "disconnect", NULL, cmd_bt_device_disconnect,
      "disconnect [mac]:generic method to disconnect" },
    { "remove", NULL, cmd_bt_remove_device, "remove [mac]:removes the remote device" },
    { "a2dp_src_run", NULL, cmd_bt_a2dp_src_run,
      "a2dp_src_run -p [folder path]  or  a2dp_src_run -f [file path]" },
    { "a2dp_src_set_status", NULL, cmd_bt_a2dp_src_set_status,
      "a2dp_src_set_status [status]:play pause forward backward" },
    { "a2dp_src_stop", NULL, cmd_bt_a2dp_src_stop, "a2dp_src_stop:stop a2dp source playing" },
    { "a2dp_set_vol", NULL, cmd_bt_a2dp_set_vol, "a2dp_set_vol: set a2dp device volme" },
    { "a2dp_get_vol", NULL, cmd_bt_a2dp_get_vol, "a2dp_src_get_vol: get a2dp device volme" },
    { "hfp_answer", NULL, cmd_bt_hfp_answer_call, "hfp_answer: answer the phone" },
    { "hfp_hangup", NULL, cmd_bt_hfp_hangup, "hfp_hangup: hangup the phone" },
    { "hfp_dial", NULL, cmd_bt_hfp_dial_num, "hfp_dial [num]: call to a phone number" },
    { "hfp_cnum", NULL, cmd_bt_hfp_subscriber_number, "hfp_cum: Subscriber Number Information" },
    { "hfp_last_num", NULL, cmd_bt_hfp_last_num_dial,
      "hfp_last_num: calling the last phone number dialed" },
    { "hfp_vol", NULL, cmd_bt_hfp_hf_vgs_volume, "hfp_vol [0~15]: update phone's volume." },
    { "hfp_at_cmd", NULL, cmd_bt_hfp_send_at_cmd, "hfp_at_cmd [cmd]: eg: hfp_at_cmd +CIEV:3,2" },
    { "hfp_ag_call", NULL, cmd_bt_hfp_ag_sco_conn, "hfp_ag_call [mac]: set sco conn" },
    { "hfp_ag_hangup", NULL, cmd_bt_hfp_ag_sco_disconn, "hfp_ag_hangup: disconnet sco" },
    { "sppc_connect", NULL, cmd_bt_spp_client_connect, "sppc_connect[mac]:connect to spp server" },
    { "sppc_send", NULL, cmd_bt_spp_client_send, "sppc_send xxx: send data" },
    { "sppc_disconnect", NULL, cmd_bt_spp_client_disconnect,
      "sppc_disconnect[mac]:disconnect spp server" },
    { "spps_accept", NULL, cmd_bt_spp_service_accept, "spps_accept the client" },
    { "spps_send", NULL, cmd_bt_spp_service_send, "spps_send data" },
    { "spps_disconnect", NULL, cmd_bt_spp_service_disconnect, "spps_disconnect dst" },
    { "ex_dbg", NULL, cmd_bt_set_ex_debug_mask, "ex_dbg [mask]: set ex debug mask" },
    { NULL, NULL, NULL, NULL },
};
