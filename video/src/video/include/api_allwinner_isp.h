#ifndef API_ALLWINNER_ISP_H_
#define API_ALLWINNER_ISP_H_

#if defined(AW_PLATFORM)
#include <cstdint>
#include <mutex>
#include <map>
#include "AWIspApi.h"

namespace ecovacs
{
    // ISP 实例状态
    struct IspInstance
    {
        int32_t isp_id;    // ISP 硬件ID
        uint32_t user_cnt; // 使用该ISP的摄像头数量

        IspInstance() : isp_id(-1), user_cnt(0) {}
    };

    class AllWinnerIsp
    {
    public:
        static AllWinnerIsp &GetInstance();

        // 启动ISP（camera_index：摄像头索引，多个摄像头可能共享同一ISP）
        int32_t Start(uint32_t camera_index);

        // 停止ISP
        int32_t Stop(uint32_t camera_index);

        // 禁止拷贝
        AllWinnerIsp(const AllWinnerIsp &) = delete;
        AllWinnerIsp &operator=(const AllWinnerIsp &) = delete;

    private:
        AllWinnerIsp() = default;
        ~AllWinnerIsp() = default;

        int32_t Init();
        int32_t Deinit();
        int32_t GetIspIdByCamera(uint32_t camera_index);

        std::mutex mutex_;                             // 线程锁
        bool inited_{false};                           // 是否已初始化
        AWIspApi *isp_api_{nullptr};                   // ISP API接口
        std::map<int32_t, IspInstance> isp_instances_; // ISP实例管理
    };
}

#endif // AW_PLATFORM
#endif // API_ALLWINNER_ISP_H_