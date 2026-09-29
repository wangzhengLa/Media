#if defined(AW_PLATFORM)
#include <type_traits>
#include <string>
#include <memory>
#include <cstdint>
#include <bits/move.h>
#include <bits/stringfwd.h>
#include <bits/char_traits.h>
#include "api_allwinner_isp.h"
#include "log.h"

namespace ecovacs
{
    AllWinnerIsp& AllWinnerIsp::GetInstance()
    {
        static AllWinnerIsp instance;
        return instance;
    }

    int32_t AllWinnerIsp::Init()
    {
        if (inited_)
            return 0;

        // 创建ISP API实例
        isp_api_ = CreateAWIspApi();
        if (!isp_api_)
        {
            ump_error("CreateAWIspApi failed");
            return -1;
        }

        // 初始化ISP
        if (isp_api_->ispApiInit && isp_api_->ispApiInit() != 0)
        {
            ump_error("ispApiInit failed");
            DestroyAWIspApi(isp_api_);
            isp_api_ = nullptr;
            return -1;
        }

        inited_ = true;
        ump_info("ISP initialized");
        return 0;
    }

    int32_t AllWinnerIsp::Deinit()
    {
        if (!inited_)
            return 0;

        // 停止所有正在使用的ISP
        for (auto& pair : isp_instances_)
        {
            int32_t isp_id = pair.first;
            if (isp_api_->ispStop)
                isp_api_->ispStop(isp_id);
            ump_info("ISP %d stopped during deinit", isp_id);
        }
        isp_instances_.clear();

        // 反初始化ISP
        if (isp_api_->ispApiUnInit)
            isp_api_->ispApiUnInit();

        DestroyAWIspApi(isp_api_);
        isp_api_ = nullptr;
        inited_ = false;

        ump_info("ISP deinitialized");
        return 0;
    }

    int32_t AllWinnerIsp::GetIspIdByCamera(uint32_t camera_index)
    {
        // 通过驱动接口获取摄像头对应的ISP ID
        if (isp_api_ && isp_api_->ispGetIspId)
            return isp_api_->ispGetIspId(camera_index);
        
        ump_error("Invalid camera index: %u", camera_index);
        return -1;
    }

    int32_t AllWinnerIsp::Start(uint32_t camera_index)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 确保已初始化
        if (!inited_ && Init() != 0)
            return -1;

        // 获取ISP ID
        int32_t isp_id = GetIspIdByCamera(camera_index);
        if (isp_id < 0)
            return -1;

        // 获取或创建ISP实例
        auto& instance = isp_instances_[isp_id];

        // 首次使用，启动ISP硬件
        if (instance.user_cnt == 0)
        {
            instance.isp_id = isp_id;
            if (isp_api_->ispStart && isp_api_->ispStart(isp_id) != 0)
            {
                ump_error("ispStart failed for ISP %d", isp_id);
                isp_instances_.erase(isp_id);
                return -1;
            }
            ump_info("ISP %d started (first user: camera %u)", isp_id, camera_index);
        }

        instance.user_cnt++;
        ump_info("Camera %u -> ISP %d, users: %u", camera_index, isp_id, instance.user_cnt);
        return 0;
    }

    int32_t AllWinnerIsp::Stop(uint32_t camera_index)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!inited_)
        {
            ump_warn("ISP not initialized");
            return -1;
        }

        // 获取ISP ID
        int32_t isp_id = GetIspIdByCamera(camera_index);
        if (isp_id < 0)
            return -1;

        // 查找ISP实例
        auto it = isp_instances_.find(isp_id);
        if (it == isp_instances_.end())
        {
            ump_warn("ISP %d not in use by camera %u", isp_id, camera_index);
            return 0;
        }

        auto& instance = it->second;
        if (instance.user_cnt == 0)
        {
            ump_warn("ISP %d already stopped", isp_id);
            isp_instances_.erase(it);
            return 0;
        }

        instance.user_cnt--;

        // 最后一个用户，停止ISP硬件
        if (instance.user_cnt == 0)
        {
            if (isp_api_->ispStop)
                isp_api_->ispStop(isp_id);
            ump_info("ISP %d stopped (last user: camera %u)", isp_id, camera_index);
            isp_instances_.erase(it);
        }
        else
        {
            ump_info("Camera %u released ISP %d, remaining users: %u",
                     camera_index, isp_id, instance.user_cnt);
        }

        return 0;
    }
}
#endif