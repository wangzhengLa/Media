#include "api_rk_isp_manager.h"
#include "log.h"
#include <unistd.h>
#include <cstring>

#ifdef ENABLE_ISP

namespace ecovacs
{

    IspManager &IspManager::GetInstance()
    {
        static IspManager instance;
        return instance;
    }

    void IspManager::InitHardware(int32_t cam_id, int32_t mainpath_dev, int32_t selfpath_dev,
                                  const std::string &camera_type, const std::string &iq_file)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (contexts_.find(cam_id) != contexts_.end())
        {
            ump_warn("ISP for cam_id %d already initialized\n", cam_id);
            return;
        }

        // 获取传感器信息（使用 cam_id）
        rk_aiq_static_info_t aiq_static_info;
        int32_t ret = rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(cam_id, &aiq_static_info);
        if (ret != 0)
        {
            ump_error("Failed to get sensor info for cam_id %d, ret=%d\n", cam_id, ret);
            return;
        }

        ump_info("Sensor info: cam_id=%d, sensor_name=%s\n",
                 cam_id, aiq_static_info.sensor_info.sensor_name);

        // 预初始化设备缓冲区
        ret = rk_aiq_uapi2_sysctl_preInit_devBufCnt(
            aiq_static_info.sensor_info.sensor_name,
            "rkraw_rx",
            2);
        if (ret != 0)
        {
            ump_warn("preInit_devBufCnt returned %d for cam_id %d\n", ret, cam_id);
        }

        // 初始化 ISP 上下文（只创建，不启动）
        rk_aiq_sys_ctx_t *aiq_ctx = rk_aiq_uapi2_sysctl_init(
            aiq_static_info.sensor_info.sensor_name,
            iq_file.c_str(),
            NULL,  // err_cb
            NULL); // sof_cb

        if (!aiq_ctx)
        {
            ump_error("Failed to init ISP for cam_id %d\n", cam_id);
            return;
        }

        // 设置多摄像头并发模式（如果有多个摄像头）
        bool is_multi_cam = (contexts_.size() > 0);
        if (is_multi_cam)
        {
            rk_aiq_uapi2_sysctl_setMulCamConc(aiq_ctx, true);
            ump_info("Multi-camera mode enabled for cam_id %d, total cameras: %zu\n",
                     cam_id, contexts_.size() + 1);
        }

        // 使用 emplace 直接构造，避免拷贝
        auto result = contexts_.emplace(cam_id, IspContext());
        IspContext &ctx = result.first->second;
        ctx.cam_id = cam_id;
        ctx.mainpath_dev = mainpath_dev;
        ctx.selfpath_dev = selfpath_dev;
        ctx.camera_type = camera_type;
        ctx.iq_file = iq_file;
        ctx.stream_count = 0;
        ctx.isp_prepared = false;
        ctx.isp_started = false;
        ctx.aiq_ctx = aiq_ctx;

        ump_info("ISP context created: cam_id=%d, mainpath=/dev/video%d, selfpath=/dev/video%d, iq=%s, multi_cam=%d\n",
                 cam_id, mainpath_dev, selfpath_dev, iq_file.c_str(), is_multi_cam);
    }

    int32_t IspManager::PrepareIsp(IspContext &ctx)
    {
        if (!ctx.aiq_ctx)
        {
            ump_error("aiq_ctx is null for cam_id %d\n", ctx.cam_id);
            return -1;
        }

        if (ctx.isp_prepared)
        {
            return 0;
        }

        // 准备 ISP（配置参数），使用普通模式
        int32_t ret = rk_aiq_uapi2_sysctl_prepare(ctx.aiq_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL);
        if (ret != 0)
        {
            ump_error("Failed to prepare ISP for cam_id %d, ret=%d\n", ctx.cam_id, ret);
            return -1;
        }

        ctx.isp_prepared = true;
        ump_debug("ISP prepared for cam_id %d\n", ctx.cam_id);
        return 0;
    }

    int32_t IspManager::StartIsp(IspContext &ctx)
    {
        if (!ctx.aiq_ctx)
        {
            ump_error("aiq_ctx is null for cam_id %d\n", ctx.cam_id);
            return -1;
        }

        if (ctx.isp_started)
        {
            return 0;
        }

        // 如果还没有 prepare，先 prepare
        if (!ctx.isp_prepared)
        {
            int32_t ret = PrepareIsp(ctx);
            if (ret != 0)
            {
                return -1;
            }
        }

        // 启动 ISP
        int32_t ret = rk_aiq_uapi2_sysctl_start(ctx.aiq_ctx);
        if (ret != 0)
        {
            ump_error("Failed to start ISP for cam_id %d, ret=%d\n", ctx.cam_id, ret);
            return -1;
        }

        ctx.isp_started = true;
        ump_info("ISP started for cam_id %d\n", ctx.cam_id);
        return 0;
    }

    int32_t IspManager::StopIsp(IspContext &ctx)
    {
        if (!ctx.aiq_ctx)
        {
            ump_error("aiq_ctx is null for cam_id %d\n", ctx.cam_id);
            return -1;
        }

        if (!ctx.isp_started)
        {
            return 0;
        }

        // 停止 ISP（false = 不等待，立即停止）
        int32_t ret = rk_aiq_uapi2_sysctl_stop(ctx.aiq_ctx, false);
        if (ret != 0)
        {
            ump_error("Failed to stop ISP for cam_id %d, ret=%d\n", ctx.cam_id, ret);
            return -1;
        }

        ctx.isp_started = false;
        ump_info("ISP stopped for cam_id %d\n", ctx.cam_id);
        return 0;
    }

    void IspManager::AddStream(int32_t cam_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = contexts_.find(cam_id);
        if (it == contexts_.end())
        {
            ump_error("AddStream: cam_id %d not found\n", cam_id);
            return;
        }

        IspContext &ctx = it->second;
        int32_t old = ctx.stream_count.fetch_add(1);

        if (old == 0)
        {
            ump_info("First stream opened for cam_id=%d, starting ISP\n", cam_id);
            StartIsp(ctx);
        }
        else
        {
            ump_debug("Stream count increased to %d for cam_id=%d\n", old + 1, cam_id);
        }
    }

    void IspManager::RemoveStream(int32_t cam_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = contexts_.find(cam_id);
        if (it == contexts_.end())
        {
            ump_error("RemoveStream: cam_id %d not found\n", cam_id);
            return;
        }

        IspContext &ctx = it->second;
        int32_t old = ctx.stream_count.fetch_sub(1);

        if (old == 1)
        {
            ump_info("Last stream closed for cam_id=%d, stopping ISP\n", cam_id);
            StopIsp(ctx);
        }
        else if (old > 1)
        {
            ump_debug("Stream count decreased to %d for cam_id=%d\n", old - 1, cam_id);
        }
        else
        {
            ump_warn("Unexpected stream_count %d for cam_id=%d\n", old, cam_id);
            ctx.stream_count = 0;
        }
    }

    void IspManager::UninitAll()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &pair : contexts_)
        {
            IspContext &ctx = pair.second;

            if (ctx.isp_started)
            {
                ump_info("Force stopping ISP for cam_id=%d\n", ctx.cam_id);
                rk_aiq_uapi2_sysctl_stop(ctx.aiq_ctx, false);
                ctx.isp_started = false;
            }

            if (ctx.aiq_ctx)
            {
                rk_aiq_uapi2_sysctl_deinit(ctx.aiq_ctx);
                ctx.aiq_ctx = nullptr;
            }

            ctx.isp_prepared = false;
            ump_info("ISP uninit: cam_id=%d\n", ctx.cam_id);
        }
        contexts_.clear();
    }

    bool IspManager::IsIspRunning(int32_t cam_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = contexts_.find(cam_id);
        if (it == contexts_.end())
            return false;
        return it->second.isp_started;
    }

    int32_t IspManager::GetStreamCount(int32_t cam_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = contexts_.find(cam_id);
        if (it == contexts_.end())
            return -1;
        return it->second.stream_count.load();
    }

    int32_t IspManager::SetFrameRate(int32_t cam_id, int32_t fps)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = contexts_.find(cam_id);
        if (it == contexts_.end())
        {
            ump_error("SetFrameRate: cam_id %d not found\n", cam_id);
            return -1;
        }

        IspContext &ctx = it->second;

        if (!ctx.aiq_ctx)
        {
            ump_error("aiq_ctx is null for cam_id %d\n", cam_id);
            return -1;
        }

        if (!ctx.isp_started)
        {
            ump_debug("ISP not started for cam_id=%d, frame rate setting deferred\n", cam_id);
            return 0;
        }

        Uapi_ExpSwAttrV2_t expSwAttr;
        memset(&expSwAttr, 0, sizeof(expSwAttr));
        int32_t ret = rk_aiq_user_api2_ae_getExpSwAttr(ctx.aiq_ctx, &expSwAttr);
        if (ret != 0)
        {
            ump_error("Failed to get exposure attributes for cam_id %d, ret=%d\n", cam_id, ret);
            return -1;
        }

        if (fps == 0)
        {
            expSwAttr.stAuto.stFrmRate.isFpsFix = false;
        }
        else
        {
            expSwAttr.stAuto.stFrmRate.isFpsFix = true;
            expSwAttr.stAuto.stFrmRate.FpsValue = fps;
        }

        ret = rk_aiq_user_api2_ae_setExpSwAttr(ctx.aiq_ctx, expSwAttr);
        if (ret != 0)
        {
            ump_error("Failed to set frame rate %d for cam_id %d, ret=%d\n", fps, cam_id, ret);
            return -1;
        }

        ump_debug("Set frame rate %d for cam_id %d\n", fps, cam_id);
        return 0;
    }

} // namespace ecovacs

#endif // ENABLE_ISP