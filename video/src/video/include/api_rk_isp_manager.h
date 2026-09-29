#ifndef ISP_MANAGER_H_
#define ISP_MANAGER_H_

#ifdef ENABLE_ISP

#include <map>
#include <atomic>
#include <mutex>
#include <string>

// 先包含 libubox 的 list.h
extern "C"
{
#include "libubox/list.h"
}

// 定义宏来重命名 libubox 的 list 函数，避免与 camera_engine_rkaiq 冲突
#define list_head _libubox_list_head
#define INIT_LIST_HEAD _libubox_INIT_LIST_HEAD
#define list_add _libubox_list_add
#define list_add_tail _libubox_list_add_tail
#define list_del _libubox_list_del
#define list_del_init _libubox_list_del_init
#define list_move _libubox_list_move
#define list_move_tail _libubox_list_move_tail
#define list_is_last _libubox_list_is_last
#define list_empty _libubox_list_empty
#define list_splice_init _libubox_list_splice_init
#define list_entry _libubox_list_entry
#define list_first_entry _libubox_list_first_entry
#define list_for_each _libubox_list_for_each
#define list_for_each_safe _libubox_list_for_each_safe
#define list_for_each_entry _libubox_list_for_each_entry
#define list_for_each_entry_safe _libubox_list_for_each_entry_safe
#define list_for_each_entry_reverse _libubox_list_for_each_entry_reverse
#define list_for_each_entry_continue _libubox_list_for_each_entry_continue
#define list_for_each_entry_continue_reverse _libubox_list_for_each_entry_continue_reverse
#define list_for_each_prev _libubox_list_for_each_prev
#define list_for_each_prev_safe _libubox_list_for_each_prev_safe

// 然后包含 RK AIQ 头文件
#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_imgproc.h"
#include "rk_aiq_user_api2_ae.h"

// 恢复宏定义
#undef list_head
#undef INIT_LIST_HEAD
#undef list_add
#undef list_add_tail
#undef list_del
#undef list_del_init
#undef list_move
#undef list_move_tail
#undef list_is_last
#undef list_empty
#undef list_splice_init
#undef list_entry
#undef list_first_entry
#undef list_for_each
#undef list_for_each_safe
#undef list_for_each_entry
#undef list_for_each_entry_safe
#undef list_for_each_entry_reverse
#undef list_for_each_entry_continue
#undef list_for_each_entry_continue_reverse
#undef list_for_each_prev
#undef list_for_each_prev_safe

namespace ecovacs
{

    class IspManager
    {
    public:
        static IspManager &GetInstance();

        // 初始化摄像头硬件
        void InitHardware(int32_t cam_id, int32_t mainpath_dev, int32_t selfpath_dev,
                          const std::string &camera_type, const std::string &iq_file);

        // 增加流引用（打开摄像头流时调用）
        void AddStream(int32_t cam_id);

        // 减少流引用（关闭摄像头流时调用）
        void RemoveStream(int32_t cam_id);

        // 释放所有资源
        void UninitAll();

        // 获取ISP状态
        bool IsIspRunning(int32_t cam_id) const;
        int32_t GetStreamCount(int32_t cam_id) const;

        // 设置帧率
        int32_t SetFrameRate(int32_t cam_id, int32_t fps);

    private:
        IspManager() = default;
        ~IspManager() = default;

        IspManager(const IspManager &) = delete;
        IspManager &operator=(const IspManager &) = delete;

        struct IspContext
        {
            int32_t cam_id;
            int32_t mainpath_dev;
            int32_t selfpath_dev;
            std::string camera_type;
            std::string iq_file;
            std::atomic<int32_t> stream_count;
            bool isp_prepared;
            bool isp_started;
            rk_aiq_sys_ctx_t *aiq_ctx;

            IspContext()
                : cam_id(-1), mainpath_dev(-1), selfpath_dev(-1), stream_count(0), isp_prepared(false), isp_started(false), aiq_ctx(nullptr) {}

            IspContext(IspContext &&other) noexcept
                : cam_id(other.cam_id), mainpath_dev(other.mainpath_dev), selfpath_dev(other.selfpath_dev), camera_type(std::move(other.camera_type)), iq_file(std::move(other.iq_file)), stream_count(other.stream_count.load()), isp_prepared(other.isp_prepared), isp_started(other.isp_started), aiq_ctx(other.aiq_ctx)
            {
                other.aiq_ctx = nullptr;
            }

            IspContext &operator=(IspContext &&other) noexcept
            {
                if (this != &other)
                {
                    cam_id = other.cam_id;
                    mainpath_dev = other.mainpath_dev;
                    selfpath_dev = other.selfpath_dev;
                    camera_type = std::move(other.camera_type);
                    iq_file = std::move(other.iq_file);
                    stream_count = other.stream_count.load();
                    isp_prepared = other.isp_prepared;
                    isp_started = other.isp_started;
                    aiq_ctx = other.aiq_ctx;
                    other.aiq_ctx = nullptr;
                }
                return *this;
            }

            IspContext(const IspContext &) = delete;
            IspContext &operator=(const IspContext &) = delete;
        };

        std::map<int32_t, IspContext> contexts_;
        mutable std::mutex mutex_;

        int32_t PrepareIsp(IspContext &ctx);
        int32_t StartIsp(IspContext &ctx);
        int32_t StopIsp(IspContext &ctx);
    };

} // namespace ecovacs

#else // ENABLE_ISP 未定义时的空实现

namespace ecovacs
{

    // 空实现，当 ISP 禁用时
    class IspManager
    {
    public:
        static IspManager &GetInstance()
        {
            static IspManager instance;
            return instance;
        }

        void InitHardware(int32_t, int32_t, int32_t, const std::string &, const std::string &) {}
        void AddStream(int32_t) {}
        void RemoveStream(int32_t) {}
        void UninitAll() {}
        bool IsIspRunning(int32_t) const { return false; }
        int32_t GetStreamCount(int32_t) const { return 0; }
        int32_t SetFrameRate(int32_t, int32_t) { return 0; }

    private:
        IspManager() = default;
        ~IspManager() = default;
        IspManager(const IspManager &) = delete;
        IspManager &operator=(const IspManager &) = delete;
    };

} // namespace ecovacs

#endif // ENABLE_ISP

#endif // ISP_MANAGER_H_