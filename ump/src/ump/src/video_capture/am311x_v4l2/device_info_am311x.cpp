/**
 * @file    device_info_am311x.cpp
 * @brief   AM311x 设备探测
 *
 * 使用 Linux Media Controller API 枚举 /dev/media* 节点，
 * 解析 CSI 拓扑图（sensor → csi → raw/eisp/isp → FR/DS 输出节点）。
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#include "device_info_am311x.h"
#include "log.h"

namespace ecovacs
{
    namespace videocapturemodule
    {

        namespace am311x_discovery
        {
            // 公开类型（camera_v2_graph, CAMERA_V2_GRAPH_CAP_*, 两个公开函数声明）
            // 已在 device_info_am311x.h 中定义，这里只需 #include 头文件即可。

            // ---- 内部常量 / 内部类型（仅 cpp 使用） ----
            constexpr size_t CAMERA_V2_MAX_ENTITIES = 128;

            struct camera_v2_media_graph
            {
                char device[CAMERA_V2_DEVICE_PATH_MAX];
                struct media_entity_desc *entities;
                size_t entity_count;
            };

            // ========================================================================
            // 内部工具函数
            // ========================================================================

            static int camera_v2_ioctl(int fd, unsigned long request, void *arg)
            {
                int ret;
                do {
                    ret = ioctl(fd, request, arg);
                } while (ret < 0 && errno == EINTR);
                return ret;
            }

            static int camera_v2_copy_string(char *destination, size_t destination_size,
                                             const char *source)
            {
                int length = snprintf(destination, destination_size, "%s", source);
                if (length < 0 || (size_t)length >= destination_size)
                    return -ENAMETOOLONG;
                return 0;
            }

            static int camera_v2_devnode_from_number(unsigned int major_number,
                                                     unsigned int minor_number,
                                                     char *device, size_t device_size)
            {
                char sysfs_path[CAMERA_V2_DEVICE_PATH_MAX];
                char line[CAMERA_V2_DEVICE_PATH_MAX];
                FILE *file;
                int ret = -ENOENT;

                if (!major_number && !minor_number)
                    return -ENODEV;

                if (snprintf(sysfs_path, sizeof(sysfs_path),
                             "/sys/dev/char/%u:%u/uevent",
                             major_number, minor_number) >= (int)sizeof(sysfs_path))
                    return -ENAMETOOLONG;

                file = fopen(sysfs_path, "r");
                if (!file)
                    return -errno;

                while (fgets(line, sizeof(line), file)) {
                    const char prefix[] = "DEVNAME=";
                    char *newline;

                    if (strncmp(line, prefix, sizeof(prefix) - 1))
                        continue;

                    newline = strchr(line, '\n');
                    if (newline)
                        *newline = '\0';

                    if (snprintf(device, device_size, "/dev/%s",
                                 line + sizeof(prefix) - 1) >= (int)device_size)
                        ret = -ENAMETOOLONG;
                    else
                        ret = 0;
                    break;
                }

                fclose(file);
                return ret;
            }

            static const struct media_entity_desc *
            camera_v2_find_named_entity(const struct camera_v2_media_graph *graph,
                                        const char *name)
            {
                for (size_t i = 0; i < graph->entity_count; i++) {
                    if (!strcmp(graph->entities[i].name, name))
                        return &graph->entities[i];
                }
                return nullptr;
            }

            static const struct media_entity_desc *
            camera_v2_find_named_video_entity(const struct camera_v2_media_graph *graph,
                                               const char *name)
            {
                for (size_t i = 0; i < graph->entity_count; i++) {
                    const struct media_entity_desc *entity = &graph->entities[i];
                    if (!strcmp(entity->name, name) &&
                        entity->type == MEDIA_ENT_F_IO_V4L &&
                        (entity->dev.major || entity->dev.minor))
                        return entity;
                }
                return nullptr;
            }

            static const struct media_entity_desc *
            camera_v2_find_named_eisp_subdev(const struct camera_v2_media_graph *graph,
                                              const char *name)
            {
                for (size_t i = 0; i < graph->entity_count; i++) {
                    const struct media_entity_desc *entity = &graph->entities[i];
                    if (!strcmp(entity->name, name) &&
                        (entity->type == MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER ||
                         entity->type == MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN))
                        return entity;
                }
                return nullptr;
            }

            static int camera_v2_read_media_graph(const char *device,
                                                   struct camera_v2_media_graph *graph)
            {
                struct media_device_info info;
                unsigned int next_id = MEDIA_ENT_ID_FLAG_NEXT;
                int fd = -1;
                int ret = 0;

                memset(graph, 0, sizeof(*graph));
                ret = camera_v2_copy_string(graph->device, sizeof(graph->device), device);
                if (ret)
                    return ret;

                fd = open(device, O_RDONLY | O_NONBLOCK);
                if (fd < 0)
                    return -errno;

                memset(&info, 0, sizeof(info));
                if (camera_v2_ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) < 0) {
                    ret = -errno;
                    goto out;
                }
                if (strcmp(info.model, "arobot-camera-v2")) {
                    ret = -ENODEV;
                    goto out;
                }

                while (graph->entity_count < CAMERA_V2_MAX_ENTITIES) {
                    struct media_entity_desc entity;
                    struct media_entity_desc *entities;

                    memset(&entity, 0, sizeof(entity));
                    entity.id = next_id;
                    if (camera_v2_ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) < 0) {
                        if (errno == EINVAL)
                            break;
                        ret = -errno;
                        goto out;
                    }

                    entities = static_cast<struct media_entity_desc *>(
                        realloc(graph->entities,
                                (graph->entity_count + 1) * sizeof(*entities)));
                    if (!entities) {
                        ret = -ENOMEM;
                        goto out;
                    }
                    graph->entities = entities;
                    graph->entities[graph->entity_count++] = entity;
                    next_id = entity.id | MEDIA_ENT_ID_FLAG_NEXT;
                }

                if (graph->entity_count == CAMERA_V2_MAX_ENTITIES) {
                    ret = -E2BIG;
                    goto out;
                }

            out:
                close(fd);
                if (ret) {
                    free(graph->entities);
                    memset(graph, 0, sizeof(*graph));
                }
                return ret;
            }

            static void camera_v2_free_media_graph(struct camera_v2_media_graph *graph)
            {
                free(graph->entities);
                memset(graph, 0, sizeof(*graph));
            }

            static int camera_v2_parse_csi_id(const char *name, const char *suffix,
                                              unsigned int *csi_id)
            {
                const char *number;
                char *end;
                unsigned long value;

                if (strncmp(name, "csi", 3))
                    return -EINVAL;

                number = name + 3;
                errno = 0;
                value = strtoul(number, &end, 10);
                if (errno || end == number || strcmp(end, suffix) || value > 255)
                    return -EINVAL;

                *csi_id = (unsigned int)value;
                return 0;
            }

            static int camera_v2_find_bound_sensor(int media_fd,
                                                    const struct camera_v2_media_graph *media,
                                                    unsigned int csi_entity_id,
                                                    const struct media_entity_desc **sensor)
            {
                for (size_t i = 0; i < media->entity_count; i++) {
                    const struct media_entity_desc *entity = &media->entities[i];
                    struct media_link_desc *links;
                    struct media_links_enum links_enum;

                    if (entity->type != MEDIA_ENT_F_CAM_SENSOR || !entity->links)
                        continue;

                    links = static_cast<struct media_link_desc *>(
                        calloc(entity->links, sizeof(*links)));
                    if (!links)
                        return -ENOMEM;

                    memset(&links_enum, 0, sizeof(links_enum));
                    links_enum.entity = entity->id;
                    links_enum.links = links;
                    if (camera_v2_ioctl(media_fd, MEDIA_IOC_ENUM_LINKS, &links_enum) < 0) {
                        int ret = -errno;
                        free(links);
                        return ret;
                    }

                    for (unsigned int j = 0; j < entity->links; j++) {
                        if (links[j].source.entity == entity->id &&
                            links[j].source.index == 0 &&
                            links[j].sink.entity == csi_entity_id &&
                            links[j].sink.index == 0 &&
                            (links[j].flags & MEDIA_LNK_FL_ENABLED)) {
                            *sensor = entity;
                            free(links);
                            return 0;
                        }
                    }

                    free(links);
                }
                return -ENODEV;
            }

            static int camera_v2_has_enabled_link(int media_fd,
                                                   const struct media_entity_desc *source,
                                                   unsigned int source_pad,
                                                   unsigned int sink_entity_id,
                                                   unsigned int sink_pad)
            {
                struct media_link_desc *links;
                struct media_links_enum links_enum;
                int ret = -ENOLINK;

                if (!source->links)
                    return -ENOLINK;

                links = static_cast<struct media_link_desc *>(
                    calloc(source->links, sizeof(*links)));
                if (!links)
                    return -ENOMEM;

                memset(&links_enum, 0, sizeof(links_enum));
                links_enum.entity = source->id;
                links_enum.links = links;
                if (camera_v2_ioctl(media_fd, MEDIA_IOC_ENUM_LINKS, &links_enum) < 0) {
                    ret = -errno;
                    goto out;
                }

                for (unsigned int i = 0; i < source->links; i++) {
                    if (links[i].source.entity == source->id &&
                        links[i].source.index == source_pad &&
                        links[i].sink.entity == sink_entity_id &&
                        links[i].sink.index == sink_pad &&
                        (links[i].flags & MEDIA_LNK_FL_ENABLED)) {
                        ret = 0;
                        break;
                    }
                }

            out:
                free(links);
                return ret;
            }

            static int camera_v2_set_video_entity(const struct media_entity_desc *entity,
                                                   char *device, size_t device_size,
                                                   char *name, size_t name_size)
            {
                int ret;
                if (!entity)
                    return -ENODEV;
                ret = camera_v2_devnode_from_number(entity->dev.major, entity->dev.minor,
                                                     device, device_size);
                if (ret)
                    return ret;
                return camera_v2_copy_string(name, name_size, entity->name);
            }

            static int camera_v2_fill_candidate(int media_fd,
                                                  const struct camera_v2_media_graph *media,
                                                  unsigned int csi_id,
                                                  struct camera_v2_graph *candidate)
            {
                const struct media_entity_desc *sensor = nullptr;
                const struct media_entity_desc *csi;
                const struct media_entity_desc *isp;
                const struct media_entity_desc *raw;
                const struct media_entity_desc *eisp_subdev;
                const struct media_entity_desc *eisp_video;
                const struct media_entity_desc *fr;
                const struct media_entity_desc *ds;
                char name[CAMERA_V2_ENTITY_NAME_MAX];
                int isp_link;
                int ret;

                memset(candidate, 0, sizeof(*candidate));

                snprintf(name, sizeof(name), "csi%u-csi", csi_id);
                csi = camera_v2_find_named_entity(media, name);
                snprintf(name, sizeof(name), "csi%u-raw", csi_id);
                raw = camera_v2_find_named_video_entity(media, name);
                snprintf(name, sizeof(name), "csi%u-eisp", csi_id);
                eisp_subdev = camera_v2_find_named_eisp_subdev(media, name);
                eisp_video = camera_v2_find_named_video_entity(media, name);
                snprintf(name, sizeof(name), "csi%u-isp", csi_id);
                isp = camera_v2_find_named_entity(media, name);
                snprintf(name, sizeof(name), "csi%u-isp-main", csi_id);
                fr = camera_v2_find_named_video_entity(media, name);
                snprintf(name, sizeof(name), "csi%u-isp-sub", csi_id);
                ds = camera_v2_find_named_video_entity(media, name);
                if (!csi)
                    return -ENODEV;

                ret = camera_v2_find_bound_sensor(media_fd, media, csi->id, &sensor);
                if (ret)
                    return ret;
                ret = camera_v2_devnode_from_number(sensor->dev.major, sensor->dev.minor,
                                                     candidate->sensor_device,
                                                     sizeof(candidate->sensor_device));
                if (ret)
                    return ret;
                candidate->csi_id = csi_id;
                if (camera_v2_copy_string(candidate->media_device,
                                          sizeof(candidate->media_device), media->device) ||
                    camera_v2_copy_string(candidate->sensor_name,
                                          sizeof(candidate->sensor_name), sensor->name))
                    return -ENAMETOOLONG;

                if (raw) {
                    ret = camera_v2_has_enabled_link(media_fd, csi, 1, raw->id, 0);
                    if (!ret) {
                        ret = camera_v2_set_video_entity(raw, candidate->raw_device,
                                                          sizeof(candidate->raw_device),
                                                          candidate->raw_name,
                                                          sizeof(candidate->raw_name));
                        if (ret)
                            return ret;
                    } else if (ret != -ENOLINK) {
                        return ret;
                    }
                }

                if (eisp_subdev && eisp_video) {
                    ret = camera_v2_has_enabled_link(media_fd, csi, 2, eisp_subdev->id, 0);
                    if (!ret)
                        ret = camera_v2_has_enabled_link(media_fd, eisp_subdev, 1,
                                                         eisp_video->id, 0);
                    if (!ret) {
                        ret = camera_v2_set_video_entity(eisp_video,
                                                          candidate->eisp_device,
                                                          sizeof(candidate->eisp_device),
                                                          candidate->eisp_name,
                                                          sizeof(candidate->eisp_name));
                        if (ret)
                            return ret;
                    } else if (ret != -ENOLINK) {
                        return ret;
                    }
                }

                if (isp) {
                    isp_link = camera_v2_has_enabled_link(media_fd, csi, 3, isp->id, 0);
                    if (isp_link && isp_link != -ENOLINK)
                        return isp_link;
                    if (!isp_link && fr) {
                        ret = camera_v2_has_enabled_link(media_fd, isp, 1, fr->id, 0);
                        if (!ret) {
                            ret = camera_v2_set_video_entity(fr, candidate->fr_device,
                                                              sizeof(candidate->fr_device),
                                                              candidate->fr_name,
                                                              sizeof(candidate->fr_name));
                            if (ret)
                                return ret;
                        } else if (ret != -ENOLINK) {
                            return ret;
                        }
                    }
                    if (!isp_link && ds) {
                        ret = camera_v2_has_enabled_link(media_fd, isp, 2, ds->id, 0);
                        if (!ret) {
                            ret = camera_v2_set_video_entity(ds, candidate->ds_device,
                                                              sizeof(candidate->ds_device),
                                                              candidate->ds_name,
                                                              sizeof(candidate->ds_name));
                            if (ret)
                                return ret;
                        } else if (ret != -ENOLINK) {
                            return ret;
                        }
                    }
                }

                return 0;
            }

            static int camera_v2_scan_one_media(const char *device,
                                                 std::vector<camera_v2_graph> &out_list)
            {
                struct camera_v2_media_graph media;
                int fd;
                int ret;

                ret = camera_v2_read_media_graph(device, &media);
                if (ret)
                    return ret;

                fd = open(device, O_RDONLY | O_NONBLOCK);
                if (fd < 0) {
                    ret = -errno;
                    goto out;
                }

                for (size_t i = 0; i < media.entity_count; i++) {
                    struct camera_v2_graph candidate;
                    unsigned int csi_id;

                    if (camera_v2_parse_csi_id(media.entities[i].name,
                                               "-csi", &csi_id))
                        continue;

                    ret = camera_v2_fill_candidate(fd, &media, csi_id, &candidate);
                    if (ret == -ENODEV)
                        continue;
                    if (ret)
                        break;

                    out_list.push_back(candidate);
                }

                close(fd);

            out:
                camera_v2_free_media_graph(&media);
                return ret;
            }

            static int camera_v2_scan_media(const char *media_device,
                                             std::vector<camera_v2_graph> &out_list)
            {
                glob_t paths;
                int ret = 0;

                out_list.clear();
                memset(&paths, 0, sizeof(paths));

                if (media_device) {
                    ret = glob(media_device, GLOB_NOCHECK, NULL, &paths);
                } else {
                    ret = glob("/dev/media*", 0, NULL, &paths);
                    if (ret == GLOB_NOMATCH)
                        return -ENOENT;
                }
                if (ret)
                    return -EINVAL;

                for (size_t i = 0; i < paths.gl_pathc; i++) {
                    ret = camera_v2_scan_one_media(paths.gl_pathv[i], out_list);
                    if (ret == -ENODEV || ret == -ENOTTY || ret == -EINVAL) {
                        ret = 0;
                        continue;
                    }
                    if (ret)
                        break;
                }

                globfree(&paths);
                if (ret) {
                    out_list.clear();
                }

                return ret;
            }

            // ========================================================================
            // V4L2 capability 查询（AM311x FR 通道）
            // ========================================================================

            struct Am311xCapability {
                int32_t width;
                int32_t height;
                int32_t maxFPS;
                video_common::VideoType videoType;
            };

            static int32_t query_fr_capabilities(const char *fr_device,
                                                  std::vector<Am311xCapability> &caps)
            {
                caps.clear();
                if (!fr_device || fr_device[0] == '\0')
                    return -ENODEV;

                ump_info("[CAP] query_fr_capabilities: opening %s\n", fr_device);

                int fd = open(fr_device, O_RDONLY);
                if (fd < 0) {
                    ump_warn("[CAP] open FR device %s failed: %s\n",
                             fr_device, strerror(errno));
                    return -errno;
                }
                ump_info("[CAP] fd=%d opened, starting ENUM_FMT...\n", fd);

                // 查询支持的像素格式
                std::vector<unsigned int> pixfmts;
                {
                    struct v4l2_fmtdesc fmtdesc;
                    memset(&fmtdesc, 0, sizeof(fmtdesc));
                    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    fmtdesc.index = 0;
                    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
                        pixfmts.push_back(fmtdesc.pixelformat);
                        fmtdesc.index++;
                    }
                }
                ump_info("[CAP] ENUM_FMT done: %zu formats\n", pixfmts.size());

                // ISP FR 输出只主推 YUYV 和 RGB3（AM311x Camera 开发指南 7.4 节），
                // NV12/NV21/UYVY 虽然可能枚举但不是常规抓帧路径，
                // 按低→高优先级排序，YUYV 最后 push 覆盖同尺寸同帧率的其他格式
                static const unsigned int interested_fmts[] = {
                    V4L2_PIX_FMT_NV12,   // 非主推
                    V4L2_PIX_FMT_NV21,   // 非主推
                    V4L2_PIX_FMT_UYVY,   // 非主推
                    V4L2_PIX_FMT_YUV420, // 非主推
                    V4L2_PIX_FMT_YUYV,   // 主推！最后 push → 优先选中
                };
                static const int32_t common_sizes[][2] = {
                    {640, 480}, {1280, 720}, {1920, 1080},
                };

                ump_info("[CAP] starting TRY_FMT loops (5 fmts x 3 sizes = 15 calls)...\n");
                int try_count = 0;
                for (auto target_fmt : interested_fmts) {
                    bool fmt_supported = false;
                    for (auto &f : pixfmts) {
                        if (f == target_fmt) { fmt_supported = true; break; }
                    }
                    if (!fmt_supported) continue;

                    for (auto &sz : common_sizes) {
                        try_count++;
                        if (try_count == 1 || try_count % 5 == 0) {
                            ump_info("[CAP] TRY_FMT #%d: fmt=%u %dx%d\n", try_count, target_fmt, sz[0], sz[1]);
                        }

                        struct v4l2_format video_fmt;
                        memset(&video_fmt, 0, sizeof(video_fmt));
                        video_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        video_fmt.fmt.pix.pixelformat = target_fmt;
                        video_fmt.fmt.pix.width = sz[0];
                        video_fmt.fmt.pix.height = sz[1];

                        if (ioctl(fd, VIDIOC_TRY_FMT, &video_fmt) >= 0) {
                            if (video_fmt.fmt.pix.width == sz[0] &&
                                video_fmt.fmt.pix.height == sz[1]) {
                                int32_t fps = 30;

                                int32_t vtype = -1;
                                switch (target_fmt) {
                                case V4L2_PIX_FMT_NV12:
                                case V4L2_PIX_FMT_NV21:
                                    vtype = (int32_t)video_common::VideoType::kNV12; break;
                                case V4L2_PIX_FMT_YUYV: vtype = (int32_t)video_common::VideoType::kYUY2; break;
                                case V4L2_PIX_FMT_UYVY: vtype = (int32_t)video_common::VideoType::kUYVY; break;
                                case V4L2_PIX_FMT_YUV420: vtype = (int32_t)video_common::VideoType::kI420; break;
                                default: continue;
                                }

                                Am311xCapability cap;
                                cap.width = sz[0];
                                cap.height = sz[1];
                                cap.maxFPS = fps;
                                cap.videoType = (video_common::VideoType)vtype;
                                caps.push_back(cap);
                            }
                        }
                    }
                }
                ump_info("[CAP] TRY_FMT done: %d calls, %zu caps found\n", try_count, caps.size());

                close(fd);
                ump_info("AM311x FR %s: discovered %zu capability combinations\n",
                         fr_device, caps.size());
                return caps.empty() ? -ENODEV : 0;
            }

            // ========================================================================
            // 公开 API
            // ========================================================================

            static void camera_v2_print_graph(const camera_v2_graph *graph)
            {
                ump_info("CSI%u media=%s sensor=%s (%s) RAW=%s EISP=%s FR=%s DS=%s\n",
                         graph->csi_id,
                         graph->media_device,
                         graph->sensor_device, graph->sensor_name,
                         graph->raw_device[0]  ? graph->raw_device  : "unavailable",
                         graph->eisp_device[0] ? graph->eisp_device : "unavailable",
                         graph->fr_device[0]   ? graph->fr_device   : "unavailable",
                         graph->ds_device[0]   ? graph->ds_device   : "unavailable");
            }

            int camera_v2_list_graphs(const char *media_device)
            {
                std::vector<camera_v2_graph> list;
                int ret = camera_v2_scan_media(media_device, list);
                if (ret)
                    return ret;

                for (auto &g : list)
                    camera_v2_print_graph(&g);

                ret = list.empty() ? -ENODEV : 0;
                return ret;
            }

            int camera_v2_discover_graph(const char *media_device,
                                         int requested_csi,
                                         unsigned int required_capabilities,
                                         camera_v2_graph *graph)
            {
                if (!graph || requested_csi < -1)
                    return -EINVAL;

                std::vector<camera_v2_graph> list;
                int ret = camera_v2_scan_media(media_device, list);
                if (ret)
                    return ret;

                const camera_v2_graph *match = nullptr;
                size_t matches = 0;

                for (auto &g : list) {
                    unsigned int capabilities = 0;

                    if (requested_csi >= 0 &&
                        g.csi_id != (unsigned int)requested_csi)
                        continue;

                    if (g.raw_device[0])
                        capabilities |= CAMERA_V2_GRAPH_CAP_RAW;
                    if (g.eisp_device[0])
                        capabilities |= CAMERA_V2_GRAPH_CAP_EISP;
                    if (g.fr_device[0] && g.ds_device[0])
                        capabilities |= CAMERA_V2_GRAPH_CAP_FULL_ISP;

                    if ((capabilities & required_capabilities) != required_capabilities)
                        continue;

                    match = &g;
                    matches++;
                }

                if (matches == 1) {
                    *graph = *match;
                    ret = 0;
                } else if (!matches) {
                    ret = -ENODEV;
                } else {
                    ump_error("multiple Camera-v2 graphs match; select one with --csi or --media-device:\n");
                    for (auto &g : list)
                        camera_v2_print_graph(&g);
                    ret = -EEXIST;
                }

                return ret;
            }

        } // namespace am311x_discovery

        // ========================================================================
        // DeviceInfoAm311x 实现
        // ========================================================================

        // 静态缓存：首次 media 扫描结果复用
        static std::vector<am311x_discovery::camera_v2_graph> s_graph_cache;

        static bool ensure_scan_done()
        {
            if (!s_graph_cache.empty())
                return true;
            int ret = am311x_discovery::camera_v2_scan_media(nullptr, s_graph_cache);
            if (ret) {
                ump_error("DeviceInfoAm311x: media scan failed: %s\n", strerror(-ret));
                return false;
            }
            ump_info("DeviceInfoAm311x: discovered %zu camera graph(s)\n", s_graph_cache.size());
            return true;
        }

        // 根据 uniqueId（即 fr_device 路径）找到对应的 graph，返回 nullptr 表示未找到
        static const am311x_discovery::camera_v2_graph *
        find_graph_by_unique_id(const char *unique_id)
        {
            for (auto &g : s_graph_cache) {
                if (strcmp(g.fr_device, unique_id) == 0)
                    return &g;
            }
            return nullptr;
        }

        DeviceInfoAm311x::DeviceInfoAm311x() : DeviceInfoImpl()
        {
        }

        DeviceInfoAm311x::~DeviceInfoAm311x()
        {
        }

        int32_t DeviceInfoAm311x::Init()
        {
            return 0;
        }

        uint32_t DeviceInfoAm311x::NumberOfDevices()
        {
            if (!ensure_scan_done())
                return 0;
            // 只统计有 FR 输出（可以做 ISP processing capture）的 graph
            uint32_t count = 0;
            for (auto &g : s_graph_cache) {
                if (g.fr_device[0] != '\0')
                    count++;
            }
            return count;
        }

        int32_t DeviceInfoAm311x::GetDeviceName(uint32_t deviceNumber,
                                                char *deviceNameUTF8,
                                                uint32_t deviceNameLength,
                                                char *deviceUniqueIdUTF8,
                                                uint32_t deviceUniqueIdUTF8Length,
                                                char *productUniqueIdUTF8,
                                                uint32_t productUniqueIdUTF8Length)
        {
            (void)productUniqueIdUTF8;
            (void)productUniqueIdUTF8Length;

            if (!ensure_scan_done())
                return -1;

            // 找第 deviceNumber 个有 FR 输出的 graph
            uint32_t idx = 0;
            for (auto &g : s_graph_cache) {
                if (g.fr_device[0] == '\0')
                    continue;
                if (idx == deviceNumber) {
                    // deviceName = 传感器名称（更有意义）
                    snprintf(deviceNameUTF8, deviceNameLength, "%s", g.sensor_name);
                    // uniqueId = fr_device 路径（后续 CreateCapabilityMap 用这个匹配 graph）
                    snprintf(deviceUniqueIdUTF8, deviceUniqueIdUTF8Length,
                             "%s", g.fr_device);
                    return 0;
                }
                idx++;
            }
            ump_error("DeviceInfoAm311x: deviceNumber=%u out of range (FR graphs=%u)\n",
                      deviceNumber, idx);
            return -1;
        }

        int32_t DeviceInfoAm311x::CreateCapabilityMap(const char *deviceUniqueIdUTF8)
        {
            // 注意：基类 GetBestMatchedCapability / NumberOfCapabilities 已经持有 _apiLock，
            // 这里不能再 lock！否则 std::mutex 非递归 → 死锁！
            ump_info("[CAP] CreateCapabilityMap ENTER: uniqueId=%s\n", deviceUniqueIdUTF8);

            if (!deviceUniqueIdUTF8) {
                ump_error("[CAP] uniqueId is null\n");
                return -1;
            }
            ump_info("[CAP] calling ensure_scan_done...\n");
            if (!ensure_scan_done()) {
                ump_error("[CAP] ensure_scan_done failed\n");
                return -1;
            }
            ump_info("[CAP] ensure_scan_done OK, s_graph_cache.size=%zu\n", s_graph_cache.size());

            ump_info("[CAP] calling find_graph_by_unique_id...\n");
            auto *graph = find_graph_by_unique_id(deviceUniqueIdUTF8);
            if (!graph) {
                ump_error("[CAP] no graph for uniqueId=%s\n", deviceUniqueIdUTF8);
                return -1;
            }
            ump_info("[CAP] graph found: fr_device=%s\n", graph->fr_device);
            if (graph->fr_device[0] == '\0') {
                ump_error("DeviceInfoAm311x: graph has no FR device\n");
                return -1;
            }

            // 从 FR V4L2 节点查询 capability
            std::vector<am311x_discovery::Am311xCapability> caps;
            int32_t ret = am311x_discovery::query_fr_capabilities(graph->fr_device, caps);
            if (ret) {
                ump_error("DeviceInfoAm311x: query_fr_capabilities failed for %s\n",
                          graph->fr_device);
                return -1;
            }

            // 填充到基类 _captureCapabilities
            _captureCapabilities.clear();
            for (auto &c : caps) {
                VideoCaptureCapability cap;
                cap.width = c.width;
                cap.height = c.height;
                cap.maxFPS = c.maxFPS;
                cap.videoType = c.videoType;
                _captureCapabilities.push_back(cap);
            }

            // 缓存 device name，避免重复查询（和 RK 保持一致）
            const uint32_t name_len = (uint32_t)strlen(deviceUniqueIdUTF8);
            _lastUsedDeviceNameLength = name_len;
            _lastUsedDeviceName = reinterpret_cast<char *>(realloc(_lastUsedDeviceName, name_len + 1));
            memcpy(_lastUsedDeviceName, deviceUniqueIdUTF8, name_len + 1);

            ump_info("DeviceInfoAm311x: CreateCapabilityMap[%zu] for %s\n",
                     _captureCapabilities.size(), deviceUniqueIdUTF8);
            return (int32_t)_captureCapabilities.size();
        }

    } // namespace videocapturemodule

} // namespace ecovacs
