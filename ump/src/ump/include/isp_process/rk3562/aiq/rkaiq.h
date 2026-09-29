#ifndef RKAIQ_H
#define RKAIQ_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h> /* getopt_long() */
#include <fcntl.h>  /* low-level i/o */
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <cf_common.h>
#include <linux/videodev2.h>
#include "common/mediactl/mediactl.h"
#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_ae.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define DBG(...)                     \
    do                               \
    {                                \
        printf("DBG: " __VA_ARGS__); \
    } while (0)
#define ERR(...)                              \
    do                                        \
    {                                         \
        fprintf(stderr, "ERR: " __VA_ARGS__); \
    } while (0)

/* Private v4l2 event */
#define CIFISP_V4L2_EVENT_STREAM_START \
    (V4L2_EVENT_PRIVATE_START + 1)
#define CIFISP_V4L2_EVENT_STREAM_STOP \
    (V4L2_EVENT_PRIVATE_START + 2)

#define RKAIQ_FILE_PATH_LEN 64
#define RKAIQ_FLASH_NUM_MAX 2

    /* 1 vicap + 2 mipi + 1 bridge + 1 redundance */
    // #define MAX_MEDIA_NODES                           2

#define IQ_PATH "/etc/iqfiles/"

    struct rkaiq_media_info
    {
        char sd_isp_path[RKAIQ_FILE_PATH_LEN];
        char vd_params_path[RKAIQ_FILE_PATH_LEN];
        char vd_stats_path[RKAIQ_FILE_PATH_LEN];
        char mainpath[RKAIQ_FILE_PATH_LEN];
        char sensor_entity_name[32];

        char mdev_path[128];
        int available;
        rk_aiq_sys_ctx_t *aiq_ctx;

        int width;
        int height;
    };

    typedef struct
    {
        bool manual_time_en;
        bool manual_gain_en;
        float time_value;
        float gain_value;
    } ManualAeAttr;

    typedef struct
    {
        unsigned char grid_matrix[225];
    } WeightsAeAttr;

    int rkaiq_get_media_info(struct rkaiq_media_info *media_info);

    int subscrible_stream_event(struct rkaiq_media_info *media_info, int fd, bool subs);

    void init_engine(struct rkaiq_media_info *media_info, int has_mul_cam);

    void start_engine(struct rkaiq_media_info *media_info);

    void stop_engine(struct rkaiq_media_info *media_info);

    void deinit_engine(struct rkaiq_media_info *media_info);

    int wait_stream_event(int fd, unsigned int event_type, int time_out_ms);

    int query_ae_state(const rk_aiq_sys_ctx_t *ctx);

    int get_ae_exp_manual(const rk_aiq_sys_ctx_t *ctx, ManualAeAttr *ae_attr, rk_aiq_uapi_mode_sync_e sync);

    int get_ae_exp_res(const rk_aiq_sys_ctx_t *ctx, ManualAeAttr *ae_attr, rk_aiq_uapi_mode_sync_e sync);

    int set_ae_weights(const rk_aiq_sys_ctx_t *ctx, BOOL enable, WeightsAeAttr *weights_ae_attr);

    int set_ae_exp_manual(const rk_aiq_sys_ctx_t *ctx, ManualAeAttr *ae_attr, rk_aiq_uapi_mode_sync_e sync);

    int set_ae_auto(const rk_aiq_sys_ctx_t *ctx, rk_aiq_uapi_mode_sync_e sync);

    int set_fixed_fps(const rk_aiq_sys_ctx_t *ctx, int fps);

#ifdef __cplusplus
}
#endif

#endif // #ifndef RKAIQ_H
