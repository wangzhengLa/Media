#include "rkaiq.h"

static void errno_exit(const char *s)
{
    ERR("%s error %d, %s\n", s, errno, strerror(errno));
}

static int xioctl(int fh, int request, void *arg)
{
    int r;

    do
    {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static int rkaiq_get_devname(struct media_device *device, const char *name, char *dev_name)
{
    const char *devname;
    struct media_entity *entity = NULL;

    entity = media_get_entity_by_name(device, name, strlen(name));
    if (!entity)
    {
        fprintf(stderr, "can't find %s device media_entity!", name);
        return -1;
    }

    devname = media_entity_get_devname(entity);
    if (!devname)
    {
        fprintf(stderr, "can't find %s device path!", name);
        return -1;
    }

    strncpy(dev_name, devname, RKAIQ_FILE_PATH_LEN);

    DBG("get %s devname: %s\n", name, dev_name);

    return 0;
}

int rkaiq_get_media_info(struct rkaiq_media_info *media_info)
{
    struct media_device *device = NULL;
    const char *sensor_name;
    int ret;

    device = media_device_new(media_info->mdev_path);
    if (!device)
        return -ENOMEM;

    /* Enumerate entities, pads and links. */
    ret = media_device_enumerate(device);
    if (ret)
        return ret;
    if (!ret)
    {
        /* Try rkisp */
        ret = rkaiq_get_devname(device, "rkisp-isp-subdev", media_info->sd_isp_path);
        ret |= rkaiq_get_devname(device, "rkisp-input-params", media_info->vd_params_path);
        ret |= rkaiq_get_devname(device, "rkisp-statistics", media_info->vd_stats_path);
        ret |= rkaiq_get_devname(device, "rkisp_mainpath", media_info->mainpath);
    }
    if (ret)
    {
        fprintf(stderr, "Cound not find rkisp dev names, skipped %s\n", media_info->mdev_path);
        media_device_unref(device);
        return ret;
    }

    sensor_name = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(media_info->mainpath);
    if (sensor_name == NULL || strlen(sensor_name) == 0)
    {
        fprintf(stderr, "ERR: No sensor attached to %s\n", media_info->mdev_path);
        media_device_unref(device);
        return -EINVAL;
    }

    strcpy(media_info->sensor_entity_name, sensor_name);

    media_device_unref(device);

    return ret;
}

int subscrible_stream_event(struct rkaiq_media_info *media_info, int fd, bool subs)
{
    struct v4l2_event_subscription sub;
    int ret = 0;

    CLEAR(sub);
    sub.type = CIFISP_V4L2_EVENT_STREAM_START;
    ret = xioctl(fd,
                 subs ? VIDIOC_SUBSCRIBE_EVENT : VIDIOC_UNSUBSCRIBE_EVENT,
                 &sub);
    if (ret)
    {
        ERR("can't subscribe %s start event!\n", media_info->vd_params_path);
    }

    CLEAR(sub);
    sub.type = CIFISP_V4L2_EVENT_STREAM_STOP;
    ret = xioctl(fd,
                 subs ? VIDIOC_SUBSCRIBE_EVENT : VIDIOC_UNSUBSCRIBE_EVENT,
                 &sub);
    if (ret)
    {
        ERR("can't subscribe %s stop event!\n", media_info->vd_params_path);
    }

    DBG("subscribe events from %s success !\n", media_info->vd_params_path);

    return 0;
}

void init_engine(struct rkaiq_media_info *media_info, int has_mul_cam)
{
    media_info->aiq_ctx = rk_aiq_uapi2_sysctl_init(media_info->sensor_entity_name, IQ_PATH, NULL, NULL);

    if (has_mul_cam)
        rk_aiq_uapi2_sysctl_setMulCamConc(media_info->aiq_ctx, 1);

    if (rk_aiq_uapi2_sysctl_prepare(media_info->aiq_ctx, media_info->width, media_info->height, RK_AIQ_WORKING_MODE_NORMAL))
    {
        ERR("rkaiq engine prepare failed !\n");
    }
}

void start_engine(struct rkaiq_media_info *media_info)
{
    DBG("device manager start\n");
    rk_aiq_uapi2_sysctl_start(media_info->aiq_ctx);
    if (media_info->aiq_ctx == NULL)
    {
        ERR("rkisp_init engine failed\n");
    }
    else
    {
        DBG("rkisp_init engine succeed\n");
    }
}

void stop_engine(struct rkaiq_media_info *media_info)
{
    rk_aiq_uapi2_sysctl_stop(media_info->aiq_ctx, false);
}

void deinit_engine(struct rkaiq_media_info *media_info)
{
    rk_aiq_uapi2_sysctl_deinit(media_info->aiq_ctx);
    if (media_info->aiq_ctx)
    {
        media_info->aiq_ctx = NULL;
    }
}

int wait_stream_event(int fd, unsigned int event_type, int time_out_ms)
{
    int ret;
    struct v4l2_event event;

    CLEAR(event);

    do
    {
        /*
         * xioctl instead of poll.
         * Since poll() cannot wait for input before stream on,
         * it will return an error directly. So, use ioctl to
         * dequeue event and block until sucess.
         */
        ret = xioctl(fd, VIDIOC_DQEVENT, &event);
        if (ret == 0 && event.type == event_type)
            return 0;
    } while (true);

    return -1;
}

int query_ae_state(const rk_aiq_sys_ctx_t *ctx)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    Uapi_ExpQueryInfo_t queryInfo;

    ret = rk_aiq_user_api2_ae_queryExpResInfo(ctx, &queryInfo);
    printf("ae IsConverged: %d\n", queryInfo.IsConverged);
    printf("ae IsExpMax: %d\n", queryInfo.IsExpMax);
    printf("ae MeanLuma: %f\n", queryInfo.MeanLuma);
    printf("ae PixelClockFreqMHZ: %f\n", queryInfo.PixelClockFreqMHZ);
    return 0;
}

int get_ae_exp_manual(const rk_aiq_sys_ctx_t *ctx, ManualAeAttr *ae_attr, rk_aiq_uapi_mode_sync_e sync)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    Uapi_ExpSwAttrV2_t expSwAttr;

    // 获取 AE 曝光软件属性
    ret = rk_aiq_user_api2_ae_getExpSwAttr(ctx, &expSwAttr);

    ae_attr->manual_time_en = expSwAttr.stManual.LinearAE.ManualTimeEn;
    ae_attr->manual_gain_en = expSwAttr.stManual.LinearAE.ManualGainEn;
    ae_attr->time_value = expSwAttr.stManual.LinearAE.TimeValue;
    ae_attr->gain_value = expSwAttr.stManual.LinearAE.GainValue;

    return 0;
}

int get_ae_exp_res(const rk_aiq_sys_ctx_t *ctx, ManualAeAttr *ae_attr, rk_aiq_uapi_mode_sync_e sync)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    Uapi_ExpQueryInfo_t queryInfo;

    // 获取 AE 内部状态信息
    ret = rk_aiq_user_api2_ae_queryExpResInfo(ctx, &queryInfo);
    ae_attr->time_value = queryInfo.CurExpInfo.LinearExp.exp_real_params.integration_time;
    ae_attr->gain_value = queryInfo.CurExpInfo.LinearExp.exp_real_params.analog_gain;
    return 0;
}

int set_ae_weights(const rk_aiq_sys_ctx_t *ctx, BOOL enable, WeightsAeAttr *weights_ae_attr)
{
    Uapi_ExpSwAttrV2_t expSwAttr;
    XCamReturn ret;
    ret = rk_aiq_user_api2_ae_getExpSwAttr(ctx, &expSwAttr);
    if (enable == TRUE)
    {
        if (weights_ae_attr != NULL)
        {
            // method one:
            memcpy(expSwAttr.GridWeights.uCoeff, weights_ae_attr->grid_matrix, sizeof(expSwAttr.GridWeights.uCoeff));

            // method two:
            expSwAttr.stAdvanced.enable = true; // important! true means preferring to use these parameters
            memcpy(expSwAttr.stAdvanced.GridWeights, weights_ae_attr->grid_matrix, sizeof(expSwAttr.stAdvanced.GridWeights));
        }
        else
        {
            printf("weights_ae_attr is null\n");
        }
    }
    else
    {
        expSwAttr.stAdvanced.enable = false;
    }
    ret = rk_aiq_user_api2_ae_setExpSwAttr(ctx, expSwAttr);
    printf("rk_aiq_user_api2_ae_setExpSwAttr ret: %d\n", ret);
    ret = rk_aiq_user_api2_ae_getExpSwAttr(ctx, &expSwAttr);
    printf("rk_aiq_user_api2_ae_setExpSwAttr ret: %d\n", ret);
    for (int i = 0; i < 15 * 15; i++)
    {
        printf(" %d", expSwAttr.GridWeights.uCoeff[i]);
    }
    printf("\n");
    return 0;
}

int set_ae_exp_manual(const rk_aiq_sys_ctx_t *ctx, ManualAeAttr *ae_attr, rk_aiq_uapi_mode_sync_e sync)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    Uapi_ExpSwAttrV2_t expSwAttr;

    // 获取 AE 曝光软件属性
    ret = rk_aiq_user_api2_ae_getExpSwAttr(ctx, &expSwAttr);
    expSwAttr.sync.sync_mode = sync;
    expSwAttr.sync.done = false;
    expSwAttr.AecOpType = RK_AIQ_OP_MODE_MANUAL;
    // LinearAE

    // 手动sensor增益使能,默认值为1
    expSwAttr.stManual.LinearAE.ManualTimeEn = ae_attr->manual_time_en;

    // 手动曝光时间使能,默认值为1
    expSwAttr.stManual.LinearAE.ManualGainEn = ae_attr->manual_gain_en;

    // 手动曝光时间值,以s为单位,参数值受sensor限制
    expSwAttr.stManual.LinearAE.TimeValue = ae_attr->time_value; /*time = 1/50s    0.02f*/

    // 手动sensor增益值,参数值受sensor限制
    expSwAttr.stManual.LinearAE.GainValue = ae_attr->gain_value; /*gain = 1x*/

    //  HdrAE (should set all frames)
    //  expSwAttr.stManual.HdrAE.ManualGainEn = true;
    //  expSwAttr.stManual.HdrAE.ManualTimeEn = true;
    //  expSwAttr.stManual.HdrAE.GainValue[0] = 1.0f; /*sframe gain = 1x*/
    //  expSwAttr.stManual.HdrAE.TimeValue[0] = 0.002f; /*sframe time = 1/500s*/
    //  expSwAttr.stManual.HdrAE.GainValue[1] = 2.0f; /*mframe gain = 2x*/
    //  expSwAttr.stManual.HdrAE.TimeValue[1] = 0.01f; /*mframe time = 1/100s*/
    //  expSwAttr.stManual.HdrAE.GainValue[2] = 4.0f; /*lframe gain = 4x*/
    //  expSwAttr.stManual.HdrAE.TimeValue[2] = 0.02f; /*lframe time = 1/50s*/
    ret = rk_aiq_user_api2_ae_setExpSwAttr(ctx, expSwAttr);

    return 0;
}

int set_fixed_fps(const rk_aiq_sys_ctx_t *ctx, int fps)
{
    int ret;
    // set fixed framemode
    Uapi_ExpSwAttrV2_t expSwAttr;
    ret = rk_aiq_user_api2_ae_getExpSwAttr(ctx, &expSwAttr);
    if (ret)
    {
        printf("rk_aiq_user_api2_ae_getExpSwAttr failed %d\n", ret);
        return ret;
    }

    expSwAttr.stAuto.stFrmRate.isFpsFix = true;
    expSwAttr.stAuto.stFrmRate.FpsValue = fps;
    ret = rk_aiq_user_api2_ae_setExpSwAttr(ctx, expSwAttr);
    return ret;
}

int set_sync_mode(const rk_aiq_sys_ctx_t *ctx, uint32_t mode)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    ret = rk_aiq_uapi2_sysctl_setSensorExpSyncMode(ctx, mode);
    return ret;
}
