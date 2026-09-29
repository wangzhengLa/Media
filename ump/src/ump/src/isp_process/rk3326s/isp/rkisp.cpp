#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h> /* getopt_long() */
#include <fcntl.h>  /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <dlfcn.h>

#include <linux/videodev2.h>
#include <rkisp_control_loop.h>
#include <rkisp_dev_manager.h>
#include <rkcamera_vendor_tags.h>
#include "mediactl.h"
#include "rkisp.h"

void *_rkisp_engine;

typedef int (*rkisp_init_func)(void **, const char *, const cl_result_callback_ops_t *);
typedef int (*rkisp_prepare_func)(void *, const struct rkisp_cl_prepare_params_s *);
typedef int (*rkisp_start_func)(void *);
typedef int (*rkisp_stop_func)(void *);
typedef int (*rkisp_deinit_func)(void *);
typedef int (*rkisp_cl_set_frame_params_func)(const void *, const struct rkisp_cl_frame_metadata_s *);

struct RKIspFunc
{
    void *rkisp_handle;
    rkisp_init_func init_func;
    rkisp_prepare_func prepare_func;
    rkisp_start_func start_func;
    rkisp_stop_func stop_func;
    rkisp_deinit_func deinit_func;
    rkisp_cl_set_frame_params_func set_frame_params_func;
};
struct RKIspFunc _RKIspFunc;

struct RKisp_media_ctl
{
    /* media controller */
    media_device *controller;
    media_entity *isp_subdev;
    media_entity *isp_params_dev;
    media_entity *isp_stats_dev;
    media_entity *sensor_subdev;
};

static char iq_file[255] = "/etc/cam_iq.xml";
static char dev_name[255] = "/dev/video0";
static int width = 640;
static int height = 480;
static int format = V4L2_PIX_FMT_NV12;
FILE *fp;

#define DBG(...)             \
    do                       \
    {                        \
        printf(__VA_ARGS__); \
    } while (0)
#define ERR(...)                      \
    do                                \
    {                                 \
        fprintf(stderr, __VA_ARGS__); \
    } while (0)

#define LIBRKISP "/usr/lib/librkisp.so"

struct control_params_3A
{
    /* used to receive current 3A settings and 3A states
     * place this at first place, so we cast ops back to base type
     */
    cl_result_callback_ops_t _result_cb_ops;
    /* used to set new setting to CL, used by _RKIspFunc.set_frame_params_func */
    rkisp_cl_frame_metadata_s _frame_metas;
    /* to manage the 3A settings, used by _frame_metas */
    CameraMetadata _settings_metadata;
    /* to manage the 3A result settings, used by metadata_result_callback */
    CameraMetadata _result_metadata;
    XCam::Mutex _meta_mutex;
};

static struct control_params_3A *g_3A_control_params = NULL;

static void metadata_result_callback(const struct cl_result_callback_ops *ops,
                                     struct rkisp_cl_frame_metadata_s *result)
{
    camera_metadata_entry entry;
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)ops;

    SmartLock lock(ctl_params->_meta_mutex);
    /* this will clone results to _result_metadata */
    ctl_params->_result_metadata = result->metas;
    DBG("meta callback!\n");
}

/*
 * construct the default camera settings, including the static
 * camera capabilities, infos.
 */
static void construct_default_metas(CameraMetadata *metas)
{
    int64_t exptime_range_ns[2] = {0, 30 * 1000 * 1000};
    int32_t sensitivity_range[2] = {0, 3200};
    uint8_t ae_mode = ANDROID_CONTROL_AE_MODE_ON;
    uint8_t control_mode = ANDROID_CONTROL_MODE_AUTO;
    uint8_t ae_lock = ANDROID_CONTROL_AE_LOCK_OFF;
    int64_t exptime_ns = 10 * 1000 * 1000;
    int32_t sensitivity = 1600;

    metas->update(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, exptime_range_ns, 2);
    metas->update(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, sensitivity_range, 2);
    metas->update(ANDROID_CONTROL_AE_MODE, &ae_mode, 1);
    metas->update(ANDROID_SENSOR_EXPOSURE_TIME, &exptime_ns, 1);
    metas->update(ANDROID_CONTROL_MODE, &control_mode, 1);
    metas->update(ANDROID_SENSOR_SENSITIVITY, &sensitivity, 1);
    metas->update(ANDROID_CONTROL_AE_LOCK, &ae_lock, 1);
}

static void init_3A_control_params()
{
    camera_metadata_t *meta;

    meta = allocate_camera_metadata(DEFAULT_ENTRY_CAP, DEFAULT_DATA_CAP);
    assert(meta);
    g_3A_control_params = new control_params_3A();
    assert(g_3A_control_params);
    g_3A_control_params->_result_cb_ops.metadata_result_callback = metadata_result_callback;
    g_3A_control_params->_settings_metadata = meta;
    construct_default_metas(&g_3A_control_params->_settings_metadata);
    g_3A_control_params->_frame_metas.id = 0;
    g_3A_control_params->_frame_metas.metas =
        g_3A_control_params->_settings_metadata.getAndLock();
    g_3A_control_params->_settings_metadata.unlock(g_3A_control_params->_frame_metas.metas);
}

static void deinit_3A_control_params()
{
    if (g_3A_control_params)
        delete g_3A_control_params;
    g_3A_control_params = NULL;
}

static int rkisp_get_meta_frame_id(void *&engine, int64_t &frame_id)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(RKCAMERA3_PRIVATEDATA_EFFECTIVE_DRIVER_FRAME_ID);
    if (!entry.count)
    {
        DBG("no RKCAMERA3_PRIVATEDATA_EFFECTIVE_DRIVER_FRAME_ID\n");
        return -1;
    }
    frame_id = entry.data.i64[0];
    DBG("meta frame id is %" PRId64 "\n", entry.data.i64[0]);

    return 0;
}

static int rkisp_get_meta_frame_sof_ts(void *&engine, int64_t &sof_ts)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(RKCAMERA3_PRIVATEDATA_FRAME_SOF_TIMESTAMP);
    if (!entry.count)
    {
        DBG("no RKCAMERA3_PRIVATEDATA_FRAME_SOF_TIMESTAMP\n");
        return -1;
    }
    sof_ts = entry.data.i64[0];
    DBG("meta frame timestamp is %" PRId64 "\n", entry.data.i64[0]);

    return 0;
}

// convenient interfaces of 3A, compatible with cifisp
static int rkisp_getAeTime(void *&engine, float &time)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_EXPOSURE_TIME);
    if (!entry.count)
        return -1;

    time = entry.data.i64[0] / (1000.0 * 1000.0 * 1000.0);
    DBG("expousre time is %f secs\n", time);

    return 0;
}

static int rkisp_getAeMaxExposureTime(void *&engine, float &time)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE);
    if (entry.count != 2)
        return -1;

    time = entry.data.i64[1] / (1000.0 * 1000.0 * 1000.0);
    DBG("expousre max time is %f secs\n", time);

    return 0;
}

static int rkisp_getAeGain(void *&engine, float &gain)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_SENSITIVITY);
    if (!entry.count)
        return -1;

    gain = (float)entry.data.i32[0] / 100;
    DBG("expousre gain is %f\n", gain);

    return 0;
}

static int rkisp_getAeMaxExposureGain(void *&engine, float &gain)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE);
    if (!entry.count)
        return -1;

    gain = entry.data.i32[1] / 100;
    DBG("expousre max gain is %f \n", gain);

    return 0;
}

static int rkisp_setAeMaxExposureTime(void *&engine, float time)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;

    int64_t exptime_range_ns[2] = {0, 0};

    exptime_range_ns[1] = time * 1000 * 1000 * 1000;
    ctl_params->_settings_metadata.update(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
                                          exptime_range_ns, 2);
    // should update new settings id
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);
    return 0;
}

static int rkisp_setAeMaxExposureGain(void *&engine, float gain)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;

    int32_t sensitivity_range[2] = {0, 0};

    sensitivity_range[1] = gain * 100;
    ctl_params->_settings_metadata.update(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
                                          sensitivity_range, 2);
    // should update new settings id
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);
    return 0;
}

static int rkisp_setManualGainAndTime(void *&engine, float hal_gain, float hal_time)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;

    int64_t exptime_ns = hal_time * 1000 * 1000 * 1000;
    // set to manual mode
    uint8_t ae_mode = ANDROID_CONTROL_AE_MODE_OFF;
    // convert to ISO100 unit
    int32_t sensitivity = hal_gain * 100;

    ctl_params->_settings_metadata.update(ANDROID_SENSOR_SENSITIVITY, &sensitivity, 1);
    ctl_params->_settings_metadata.update(ANDROID_CONTROL_AE_MODE, &ae_mode, 1);
    ctl_params->_settings_metadata.update(ANDROID_SENSOR_EXPOSURE_TIME, &exptime_ns, 1);
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);

    return 0;
}

static int rkisp_setAeMode(void *&engine, HAL_AE_OPERATION_MODE mode)
{
    struct control_params_3A *ctl_params =
        (struct control_params_3A *)engine;
    // set to manual mode
    uint8_t ae_mode = ANDROID_CONTROL_AE_MODE_OFF;

    if (mode == HAL_AE_OPERATION_MODE_AUTO)
    {
        ae_mode = ANDROID_CONTROL_AE_MODE_ON;
        ctl_params->_settings_metadata.update(ANDROID_CONTROL_AE_MODE, &ae_mode, 1);
    }
    else if (mode == HAL_AE_OPERATION_MODE_MANUAL)
    {
        camera_metadata_entry entry;
        SmartLock lock(ctl_params->_meta_mutex);

        entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_EXPOSURE_TIME);
        if (!entry.count)
            return -1;
        ctl_params->_settings_metadata.update(ANDROID_SENSOR_EXPOSURE_TIME, entry.data.i64, 1);

        entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_SENSITIVITY);
        if (!entry.count)
            return -1;
        ctl_params->_settings_metadata.update(ANDROID_SENSOR_SENSITIVITY, entry.data.i32, 1);

        ctl_params->_settings_metadata.update(ANDROID_CONTROL_AE_MODE, &ae_mode, 1);
    }
    else
    {
        ERR("unsupported ae mode %d\n", mode);
        return -1;
    }
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);

    return 0;
}

static struct media_device *__rkisp_get_media_dev_by_vnode(const char *vnode)
{
    char sys_path[64];
    struct media_device *device = NULL;
    uint32_t nents, j, i = 0;
    FILE *fp;
    int ret;

    while (i < 64)
    {
        snprintf(sys_path, 64, "/dev/media%d", i++);
        fp = fopen(sys_path, "r");
        if (!fp)
            continue;
        fclose(fp);

        device = media_device_new(sys_path);

        /* Enumerate entities, pads and links. */
        media_device_enumerate(device);

        nents = media_get_entities_count(device);
        for (j = 0; j < nents; ++j)
        {
            struct media_entity *entity = media_get_entity(device, j);
            const char *devname = media_entity_get_devname(entity);
            if (NULL != devname)
            {
                if (!strcmp(devname, vnode))
                {
                    goto out;
                }
            }
        }
        media_device_unref(device);
    }

out:
    return device;
}

manual_info_t set_3A_params(float gain, float expo)
{
    manual_info_t info;
    float exptime, expgain;
    int64_t frame_id, frame_sof;

    if (gain > 0 && expo > 0)
        rkisp_setManualGainAndTime((void *&)g_3A_control_params, gain, expo);
    else
        rkisp_setAeMode((void *&)g_3A_control_params, HAL_AE_OPERATION_MODE_AUTO);

    rkisp_getAeTime((void *&)g_3A_control_params, exptime);
    info.AeTime = exptime;
    rkisp_getAeGain((void *&)g_3A_control_params, expgain);
    info.AeGain = expgain;
    rkisp_getAeMaxExposureGain((void *&)g_3A_control_params, expgain);
    info.AeMaxGain = expgain;
    rkisp_getAeMaxExposureTime((void *&)g_3A_control_params, exptime);
    info.AeMaxTime = exptime;
    rkisp_get_meta_frame_id((void *&)g_3A_control_params, frame_id);
    rkisp_get_meta_frame_sof_ts((void *&)g_3A_control_params, frame_sof);

    return info;
}

void stop_engine(void)
{
    if (_RKIspFunc.stop_func != NULL)
    {
        DBG("stop rkisp engine\n");
        _RKIspFunc.stop_func(_rkisp_engine);
    }
}

void start_engine(void)
{
    if (_RKIspFunc.start_func != NULL)
    {
        DBG("device manager start\n");
        _RKIspFunc.start_func(_rkisp_engine);
        DBG("device manager isp_init\n");

        if (_rkisp_engine == NULL)
        {
            ERR("rkisp_init engine failed\n");
        }
        else
        {
            DBG("rkisp_init engine succeed\n");
        }
    }
}

void init_engine(void **_engine)
{
    unsigned int i;
    struct RKisp_media_ctl rkisp;
    enum v4l2_buf_type type;

    if (_RKIspFunc.init_func != NULL)
    {
        _RKIspFunc.init_func(&_rkisp_engine, iq_file,
                             (cl_result_callback_ops_t *)(g_3A_control_params));
    }
    if (_RKIspFunc.prepare_func != NULL)
    {
        struct rkisp_cl_prepare_params_s params = {0};
        unsigned int nents;

        rkisp.controller =
            __rkisp_get_media_dev_by_vnode(dev_name);
        if (!rkisp.controller)
            printf("Can't find controller, maybe use a wrong video-node or wrong permission to media node");
        rkisp.isp_subdev =
            media_get_entity_by_name(rkisp.controller, "rkisp1-isp-subdev",
                                     strlen("rkisp1-isp-subdev"));
        rkisp.isp_params_dev =
            media_get_entity_by_name(rkisp.controller, "rkisp1-input-params",
                                     strlen("rkisp1-input-params"));
        rkisp.isp_stats_dev =
            media_get_entity_by_name(rkisp.controller, "rkisp1-statistics",
                                     strlen("rkisp1-statistics"));
        /* TODO: Get ACTIVE subdev sensor */
        nents = media_get_entities_count(rkisp.controller);
        rkisp.sensor_subdev = NULL;
        for (i = 0; i < nents; ++i)
        {
            struct media_entity *e;
            const struct media_entity_desc *ef;

            e = media_get_entity(rkisp.controller, i);
            ef = media_entity_get_info(e);
            if (ef->type == MEDIA_ENT_T_V4L2_SUBDEV_SENSOR)
            {
                rkisp.sensor_subdev = e;
                break;
            }
        }
        assert(rkisp.sensor_subdev);

        params.isp_sd_node_path = media_entity_get_devname(rkisp.isp_subdev);
        params.isp_vd_params_path = media_entity_get_devname(rkisp.isp_params_dev);
        params.isp_vd_stats_path = media_entity_get_devname(rkisp.isp_stats_dev);
        params.sensor_sd_node_path = media_entity_get_devname(rkisp.sensor_subdev);
        params.staticMeta =
            g_3A_control_params->_settings_metadata.getAndLock();
        _RKIspFunc.prepare_func(_rkisp_engine, &params);
        g_3A_control_params->_settings_metadata.unlock(params.staticMeta);
        media_device_unref(rkisp.controller);
    }

    *_engine = _rkisp_engine;
}

void uninit_3A_params(void)
{
    if (_RKIspFunc.deinit_func != NULL)
    {
        _RKIspFunc.deinit_func(_rkisp_engine);
        deinit_3A_control_params();
    }
    if (_RKIspFunc.rkisp_handle)
        dlclose(_RKIspFunc.rkisp_handle);
}

void init_3A_params(void)
{
    // INIT RKISP
    _RKIspFunc.rkisp_handle = dlopen(LIBRKISP, RTLD_NOW);
    if (_RKIspFunc.rkisp_handle == NULL)
    {
        ERR("open %s failed\n", LIBRKISP);
    }
    else
    {
        DBG("open %s successed\n", LIBRKISP);
        _RKIspFunc.init_func = (rkisp_init_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_init");
        _RKIspFunc.prepare_func = (rkisp_prepare_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_prepare");
        _RKIspFunc.start_func = (rkisp_start_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_start");
        _RKIspFunc.stop_func = (rkisp_stop_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_stop");
        _RKIspFunc.deinit_func = (rkisp_deinit_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_deinit");
        _RKIspFunc.set_frame_params_func = (rkisp_cl_set_frame_params_func)dlsym(_RKIspFunc.rkisp_handle,
                                                                                 "rkisp_cl_set_frame_params");
        if (_RKIspFunc.start_func == NULL)
        {
            ERR("func rkisp_start not found.\n");
            const char *errmsg;
            if ((errmsg = dlerror()) != NULL)
            {
                ERR("dlsym rkisp_start fail errmsg: %s\n", errmsg);
            }
        }
        else
        {
            DBG("dlsym rkisp_start success\n");
        }
        init_3A_control_params();
    }
}
