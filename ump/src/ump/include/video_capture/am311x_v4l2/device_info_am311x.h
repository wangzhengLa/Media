#ifndef DEVICE_INFO_AM311X_H_
#define DEVICE_INFO_AM311X_H_

#include <stddef.h>
#include <stdint.h>
#include "device_info_impl.h"

namespace ecovacs
{
    namespace videocapturemodule
    {

        /**
         * @brief AM311x Camera-v2 discovery 公开 API
         *
         * 移植自 am311x_open_sdk/linux/buildroot/package/am-camera/src/camera_v2_discovery.h
         * 用于 demo 层或上层业务按 CSI ID / capability 精确匹配 camera graph。
         */
        namespace am311x_discovery
        {
            constexpr size_t CAMERA_V2_DEVICE_PATH_MAX = 128;
            constexpr size_t CAMERA_V2_ENTITY_NAME_MAX = 64;

            /// graph 能力标志位（可按位组合）
            constexpr unsigned int CAMERA_V2_GRAPH_CAP_RAW     = (1U << 0);
            constexpr unsigned int CAMERA_V2_GRAPH_CAP_EISP    = (1U << 1);
            constexpr unsigned int CAMERA_V2_GRAPH_CAP_FULL_ISP = (1U << 2);

            /// 单个 camera graph 的完整拓扑信息（移植自原 camera_v2_graph）
            struct camera_v2_graph
            {
                unsigned int csi_id;
                char media_device[CAMERA_V2_DEVICE_PATH_MAX];
                char sensor_device[CAMERA_V2_DEVICE_PATH_MAX];
                char sensor_name[CAMERA_V2_ENTITY_NAME_MAX];
                char raw_device[CAMERA_V2_DEVICE_PATH_MAX];
                char raw_name[CAMERA_V2_ENTITY_NAME_MAX];
                char eisp_device[CAMERA_V2_DEVICE_PATH_MAX];
                char eisp_name[CAMERA_V2_ENTITY_NAME_MAX];
                char fr_device[CAMERA_V2_DEVICE_PATH_MAX];
                char fr_name[CAMERA_V2_ENTITY_NAME_MAX];
                char ds_device[CAMERA_V2_DEVICE_PATH_MAX];
                char ds_name[CAMERA_V2_ENTITY_NAME_MAX];
            };

            /**
             * @brief 扫描所有 /dev/media*，列出发现的每个 graph（ump_info 输出）
             * @param media_device 可选 glob 模式（如 "/dev/media*"），传 nullptr 则默认扫描全部
             * @return 0 表示至少发现一个，-ENODEV 表示未发现任何设备
             */
            int camera_v2_list_graphs(const char *media_device);

            /**
             * @brief 按条件精确发现单个 camera graph
             * @param media_device        可选 glob 模式，nullptr = 扫 /dev/media*
             * @param requested_csi       指定 CSI ID（>=0）；传 -1 表示任意 CSI
             * @param required_capabilities  必须具备的 capability 位掩码
             * @param graph               输出：匹配到的 graph（成功时）
             * @return 0 成功；-ENODEV 无匹配；-EEXIST 多个匹配；负值 errno
             */
            int camera_v2_discover_graph(const char *media_device,
                                         int requested_csi,
                                         unsigned int required_capabilities,
                                         camera_v2_graph *graph);
        } // namespace am311x_discovery

        // ============================================================================
        // DeviceInfoAm311x — ump 框架的 DeviceInfo 实现
        // ============================================================================

        class DeviceInfoAm311x : public DeviceInfoImpl
        {
        public:
            DeviceInfoAm311x();
            ~DeviceInfoAm311x() override;

            uint32_t NumberOfDevices() override;

            int32_t GetDeviceName(uint32_t deviceNumber,
                                  char *deviceNameUTF8,
                                  uint32_t deviceNameLength,
                                  char *deviceUniqueIdUTF8,
                                  uint32_t deviceUniqueIdUTF8Length,
                                  char *productUniqueIdUTF8 = 0,
                                  uint32_t productUniqueIdUTF8Length = 0) override;

        protected:
            int32_t Init() override;
            int32_t CreateCapabilityMap(const char *deviceUniqueIdUTF8) override;
        };

    } // namespace videocapturemodule

} // namespace ecovacs

#endif // DEVICE_INFO_AM311X_H_
