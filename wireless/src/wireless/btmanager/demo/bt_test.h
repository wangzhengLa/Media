/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __BT_TEST_H__
#define __BT_TEST_H__

#include "bt_dev_list.h"
#include <byteswap.h>

/* Definitions for Microsoft WAVE format */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define COMPOSE_ID(a,b,c,d)	((a) | ((b)<<8) | ((c)<<16) | ((d)<<24))
#define LE_SHORT(v)		(v)
#define LE_INT(v)		(v)
#define BE_SHORT(v)		bswap_16(v)
#define BE_INT(v)		bswap_32(v)
#elif __BYTE_ORDER == __BIG_ENDIAN
#define COMPOSE_ID(a,b,c,d)	((d) | ((c)<<8) | ((b)<<16) | ((a)<<24))
#define LE_SHORT(v)		bswap_16(v)
#define LE_INT(v)		bswap_32(v)
#define BE_SHORT(v)		(v)
#define BE_INT(v)		(v)
#else
#error "Wrong endian"
#endif

/* Note: the following macros evaluate the parameter v twice */
#define TO_CPU_SHORT(v, be) \
    ((be) ? BE_SHORT(v) : LE_SHORT(v))
#define TO_CPU_INT(v, be) \
    ((be) ? BE_INT(v) : LE_INT(v))

#define WAV_RIFF		COMPOSE_ID('R','I','F','F')
#define WAV_RIFX		COMPOSE_ID('R','I','F','X')
#define WAV_WAVE		COMPOSE_ID('W','A','V','E')
#define WAV_FMT			COMPOSE_ID('f','m','t',' ')
#define WAV_DATA		COMPOSE_ID('d','a','t','a')

/* WAVE fmt block constants from Microsoft mmreg.h header */
#define WAV_FMT_PCM             0x0001
#define WAV_FMT_IEEE_FLOAT      0x0003
#define WAV_FMT_DOLBY_AC3_SPDIF 0x0092
#define WAV_FMT_EXTENSIBLE      0xfffe

/* Used with WAV_FMT_EXTENSIBLE format */
#define WAV_GUID_TAG		"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71"

/* it's in chunks like .voc and AMIGA iff, but my source say there
   are in only in this combination, so I combined them in one header;
   it works on all WAVE-file I have
 */
typedef struct {
	u_int magic;		/* 'RIFF' */
	u_int length;		/* filelen */
	u_int type;		/* 'WAVE' */
} WaveHeader;

typedef struct {
	u_short format;		/* see WAV_FMT_* */
	u_short channels;
	u_int sample_fq;	/* frequence of sample */
	u_int byte_p_sec;
	u_short byte_p_spl;	/* samplesize; 1 or 2 bytes */
	u_short bit_p_spl;	/* 8, 12 or 16 bit */
} WaveFmtBody;

typedef struct {
	WaveFmtBody format;
	u_short ext_size;
	u_short bit_p_spl;
	u_int channel_mask;
	u_short guid_format;	/* WAV_FMT_* */
	u_char guid_tag[14];	/* WAV_GUID_TAG */
} WaveFmtExtensibleBody;

typedef struct {
	u_int type;		/* 'data' */
	u_int length;		/* samplecount */
} WaveChunkHeader;

typedef struct {
    const char *cmd;
    const char *arg;
    void (*func)(int argc, char *args[]);
    const char *desc;
} cmd_tbl_t;

extern int config_ext_adv;
extern dev_list_t *discovered_devices;
extern btmg_profile_info_t *bt_pro_info;

extern cmd_tbl_t common_cmd_table[];
extern cmd_tbl_t bt_cmd_table[];
extern cmd_tbl_t bt_gatts_cmd_table[];
extern cmd_tbl_t bt_gattc_cmd_table[];

/*gatt client*/
void bt_gatt_client_register_callback(btmg_callback_t *cb);
int bt_gatt_client_init();
int bt_gatt_client_deinit();

/*gatt server*/
void bt_gatt_server_register_callback(btmg_callback_t *cb);
int bt_gatt_server_init();
int bt_gatt_server_deinit();

/*a2dp src*/
bool bt_a2dp_src_is_run(void);
void bt_a2dp_src_stop(void);

#include <stddef.h>
#include <stdio.h>
#include <errno.h>

#undef MIN
#undef MAX
#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))

#if 1
static int char2hex(char c, uint8_t *x)
{
    if (c >= '0' && c <= '9') {
        *x = c - '0';
    } else if (c >= 'a' && c <= 'f') {
        *x = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        *x = c - 'A' + 10;
    } else {
        return -EINVAL;
    }

    return 0;
}

static int hex2char(uint8_t x, char *c)
{
    if (x <= 9) {
        *c = x + '0';
    } else if (x >= 10 && x <= 15) {
        *c = x - 10 + 'a';
    } else {
        return -EINVAL;
    }

    return 0;
}

static size_t hex2bin(const char *hex, size_t hexlen, uint8_t *buf, size_t buflen)
{
    uint8_t dec;

    if (buflen < hexlen / 2 + hexlen % 2) {
        return 0;
    }

    /* if hexlen is uneven, insert leading zero nibble */
    if (hexlen % 2) {
        if (char2hex(hex[0], &dec) < 0) {
            return 0;
        }
        buf[0] = dec;
        hex++;
        buf++;
    }

    /* regular hex conversion */
    for (size_t i = 0; i < hexlen / 2; i++) {
        if (char2hex(hex[2 * i], &dec) < 0) {
            return 0;
        }
        buf[i] = dec << 4;

        if (char2hex(hex[2 * i + 1], &dec) < 0) {
            return 0;
        }
        buf[i] += dec;
    }

    return hexlen / 2 + hexlen % 2;
}

static size_t bin2hex(const uint8_t *buf, size_t buflen, char *hex, size_t hexlen)
{
    if ((hexlen + 1) < buflen * 2) {
        return 0;
    }

    for (size_t i = 0; i < buflen; i++) {
        if (hex2char(buf[i] >> 4, &hex[2 * i]) < 0) {
            return 0;
        }
        if (hex2char(buf[i] & 0xf, &hex[2 * i + 1]) < 0) {
            return 0;
        }
    }

    hex[2 * buflen] = '\0';
    return 2 * buflen;
}
#endif

#endif