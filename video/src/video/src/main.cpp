#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <memory>
#include <thread>
#include <chrono>
#include <fstream>

extern "C"
{
#include "libubox/blobmsg_json.h"
#include "libubox/uloop.h"
#include "libubox/utils.h"
#include "libubus.h"
}

#include "api_media_engine.h"
#include "video_common_defines.h"
#include "frame_subscriber.h"
#include "image_subscriber.h"
#include "string_subscriber.h"
#include "log.h"
#include "osp.h"
#include "inproc_comm.h"
#include "api_video_ai_process.h"

#ifdef ENABLE_ICEORYX
#include "iceoryx_posh/runtime/posh_runtime.hpp"
#include "iceoryx_posh/popo/subscriber.hpp"
#include "iceoryx_posh/popo/listener.hpp"
#include "iceoryx_posh/popo/notification_callback.hpp"
#endif

#define PID_FILE "/var/run/video.pid"

// ========== 常量定义 ==========
constexpr int GRAP_TIMEOUT_SEC = 4;
constexpr int GRAP_CHECK_INTERVAL_MS = 50;
constexpr int GRAP_MAX_RETRIES = (GRAP_TIMEOUT_SEC * 1000) / GRAP_CHECK_INTERVAL_MS;

// ========== 全局变量 ==========
static std::mutex g_main_lock; // ubus回调互斥锁
static int g_pid_fd = -1;      // PID文件描述符（用于atexit清理）

/***************************************ubus控制命令实现**************************************/
enum grap_param
{
    FILE_PATH,
    __GRAP_PARAM_MAX
};
enum set_bitrate_param
{
    BIT_RATE,
    __BIT_RATE_PARAM_MAX
};

static const struct blobmsg_policy grap_policy[] = {
    {.name = "file_path", .type = BLOBMSG_TYPE_STRING},
};
static const struct blobmsg_policy set_bitrate_policy[] = {
    {.name = "bit_rate", .type = BLOBMSG_TYPE_INT32},
};

static int grap_pic_cb(struct ubus_context *ctx, struct ubus_object *obj,
                       struct ubus_request_data *req, const char *method,
                       struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("grab_pic");

    struct blob_buf b = {0};
    struct blob_attr *tb[__GRAP_PARAM_MAX];
    char *file_path = NULL;

    blobmsg_parse(grap_policy, ARRAY_SIZE(grap_policy), tb, blob_data(msg), blob_len(msg));

    if (tb[FILE_PATH])
        file_path = blobmsg_get_string(tb[FILE_PATH]);

    if (!file_path)
    {
        blob_buf_init(&b, 0);
        blobmsg_add_string(&b, "ret", "failed");
        blobmsg_add_string(&b, "error", "missing file_path");
        ubus_send_reply(ctx, req, b.head);
        return -1;
    }

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (!engine)
    {
        blob_buf_init(&b, 0);
        blobmsg_add_string(&b, "ret", "failed");
        blobmsg_add_string(&b, "error", "media engine not initialized");
        ubus_send_reply(ctx, req, b.head);
        return -1;
    }

    engine->grap_status_ = ecovacs::GrapStatus::START;

    ecovacs::video_common::VideoJpegGrap grap_config;
    grap_config.file_path = file_path;
    engine->StartTakePhotograph(grap_config);

    uint32_t query_count = 0;
    bool grap_flag = false;
    while (true)
    {
        if (ecovacs::GrapStatus::FINISH == engine->grap_status_)
            grap_flag = true;
        if (query_count++ >= GRAP_MAX_RETRIES || grap_flag)
            break;
        IntelliUtilSleepMs(GRAP_CHECK_INTERVAL_MS);
    }

    blob_buf_init(&b, 0);
    if (grap_flag)
    {
        blobmsg_add_string(&b, "ret", "ok");
        blobmsg_add_string(&b, "file_path", file_path);
    }
    else
    {
        blobmsg_add_string(&b, "ret", "failed");
        blobmsg_add_string(&b, "error", "grap timeout");
    }
    ubus_send_reply(ctx, req, b.head);

    engine->StopTakePhotograph();
    engine->grap_status_ = ecovacs::GrapStatus::IDEL;
    return 0;
}

static int save_raw_video_cb(struct ubus_context *ctx, struct ubus_object *obj,
                             struct ubus_request_data *req, const char *method,
                             struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("save_raw_video");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (!engine)
    {
        struct blob_buf b = {0};
        blob_buf_init(&b, 0);
        blobmsg_add_string(&b, "ret", "failed");
        blobmsg_add_string(&b, "error", "media engine not initialized");
        ubus_send_reply(ctx, req, b.head);
        return -1;
    }

    engine->StartSaveRawVideo();

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int request_idr_cb(struct ubus_context *ctx, struct ubus_object *obj,
                          struct ubus_request_data *req, const char *method,
                          struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("request_idr");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (!engine)
    {
        struct blob_buf b = {0};
        blob_buf_init(&b, 0);
        blobmsg_add_string(&b, "ret", "failed");
        blobmsg_add_string(&b, "error", "media engine not initialized");
        ubus_send_reply(ctx, req, b.head);
        return -1;
    }

    auto *encode_element = engine->video_encode_element_.get();
    if (!encode_element)
    {
        struct blob_buf b = {0};
        blob_buf_init(&b, 0);
        blobmsg_add_string(&b, "ret", "failed");
        blobmsg_add_string(&b, "error", "encode element not initialized");
        ubus_send_reply(ctx, req, b.head);
        return -1;
    }

    auto *h264_encode = encode_element->video_h264_encode_.get();
    if (h264_encode && h264_encode->EncodeStarted())
    {
        h264_encode->RequestKeyFrame();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int set_bitrate_cb(struct ubus_context *ctx, struct ubus_object *obj,
                          struct ubus_request_data *req, const char *method,
                          struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("set_bitrate");

    struct blob_buf b = {0};
    struct blob_attr *tb[__BIT_RATE_PARAM_MAX];
    uint32_t bit_rate = 0;

    blobmsg_parse(set_bitrate_policy, ARRAY_SIZE(set_bitrate_policy), tb,
                  blob_data(msg), blob_len(msg));

    if (tb[BIT_RATE])
        bit_rate = blobmsg_get_u32(tb[BIT_RATE]);

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (!engine)
    {
        blob_buf_init(&b, 0);
        blobmsg_add_string(&b, "ret", "failed");
        blobmsg_add_string(&b, "error", "media engine not initialized");
        ubus_send_reply(ctx, req, b.head);
        return -1;
    }

    ecovacs::video_common::RateControlParameters parameters;
    parameters.framerate_fps = 30;
    parameters.target_bitrate = bit_rate;

    auto *encode_element = engine->video_encode_element_.get();
    if (encode_element)
    {
        auto *h264_encode = encode_element->video_h264_encode_.get();
        if (h264_encode && h264_encode->EncodeStarted())
        {
            h264_encode->SetRates(parameters);
        }
    }

    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int start_ai_cb(struct ubus_context *ctx, struct ubus_object *obj,
                       struct ubus_request_data *req, const char *method,
                       struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("start_ai");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StartAI();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int stop_ai_cb(struct ubus_context *ctx, struct ubus_object *obj,
                      struct ubus_request_data *req, const char *method,
                      struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("stop_ai");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StopAI();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int start_push_cb(struct ubus_context *ctx, struct ubus_object *obj,
                         struct ubus_request_data *req, const char *method,
                         struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("start_push");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StartPush();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int stop_push_cb(struct ubus_context *ctx, struct ubus_object *obj,
                        struct ubus_request_data *req, const char *method,
                        struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("stop_push");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StopPush();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int start_rtsp_push_cb(struct ubus_context *ctx, struct ubus_object *obj,
                         struct ubus_request_data *req, const char *method,
                         struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("start_push");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StartRtspPush();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int stop_rtsp_push_cb(struct ubus_context *ctx, struct ubus_object *obj,
                        struct ubus_request_data *req, const char *method,
                        struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("stop_push");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StopRtspPush();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

#if defined(ENABLE_WEBRTC)
static int start_webrtc_push_cb(struct ubus_context *ctx, struct ubus_object *obj,
                                struct ubus_request_data *req, const char *method,
                                struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("start_webrtc_push");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StartWebrtcPush();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int stop_webrtc_push_cb(struct ubus_context *ctx, struct ubus_object *obj,
                               struct ubus_request_data *req, const char *method,
                               struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("stop_webrtc_push");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StopWebrtcPush();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}
#endif

#if defined(ENABLE_LIBPEER)
static int start_libpeer_push_cb(struct ubus_context *ctx, struct ubus_object *obj,
                                 struct ubus_request_data *req, const char *method,
                                 struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("start_libpeer_push");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StartLibpeerPush();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int stop_libpeer_push_cb(struct ubus_context *ctx, struct ubus_object *obj,
                                struct ubus_request_data *req, const char *method,
                                struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("stop_libpeer_push");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StopLibpeerPush();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}
#endif

static int start_jpeg_pub_cb(struct ubus_context *ctx, struct ubus_object *obj,
                             struct ubus_request_data *req, const char *method,
                             struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("start_jpeg_pub");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StartJpegPublish();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static int stop_jpeg_pub_cb(struct ubus_context *ctx, struct ubus_object *obj,
                            struct ubus_request_data *req, const char *method,
                            struct blob_attr *msg)
{
    std::lock_guard<std::mutex> guard(g_main_lock);
    ump_info("stop_jpeg_pub");

    auto *engine = ecovacs::MediaEngine::MediaEngineInstance();
    if (engine)
    {
        engine->StopJpegPublish();
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "ret", "ok");
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

// ========== ubus 方法定义 ==========
static struct ubus_method control_methods[] = {
    {.name = "grap_pic", .handler = grap_pic_cb},
    {.name = "save_raw_video", .handler = save_raw_video_cb},
    {.name = "request_idr", .handler = request_idr_cb},
    {.name = "set_bitrate", .handler = set_bitrate_cb},
    {.name = "start_ai", .handler = start_ai_cb},
    {.name = "stop_ai", .handler = stop_ai_cb},
    {.name = "start_push", .handler = start_push_cb},
    {.name = "stop_push", .handler = stop_push_cb},
    {.name = "start_rtsp_push", .handler = start_rtsp_push_cb},
    {.name = "stop_rtsp_push", .handler = stop_rtsp_push_cb},
#if defined(ENABLE_LIBPEER)
    {.name = "start_libpeer_push", .handler = start_libpeer_push_cb},
    {.name = "stop_libpeer_push", .handler = stop_libpeer_push_cb},
#endif
#if defined(ENABLE_WEBRTC)
    {.name = "start_webrtc_push", .handler = start_webrtc_push_cb},
    {.name = "stop_webrtc_push", .handler = stop_webrtc_push_cb},
#endif
    {.name = "start_jpeg_pub", .handler = start_jpeg_pub_cb},
    {.name = "stop_jpeg_pub", .handler = stop_jpeg_pub_cb},
};

static struct ubus_object_type control_type = {
    .name = "video.control",
    .methods = control_methods,
    .n_methods = ARRAY_SIZE(control_methods),
};

static struct ubus_object control_obj = {
    .name = "video.control",
    .type = &control_type,
    .methods = control_methods,
    .n_methods = ARRAY_SIZE(control_methods),
};

static void ubus_reconn_timer_cb(struct uloop_timeout *timeout, struct ubus_context *ctx)
{
    static struct uloop_timeout reconn_timer = {0};
    reconn_timer.cb = [](struct uloop_timeout *t)
    {
        ubus_reconn_timer_cb(t, nullptr);
    };

    if (ubus_reconnect(ctx, ecovacs::MediaEngine::MediaEngineInstance()
                                ->media_engine_cfg_.unix_socket_path) != 0)
    {
        ump_error("reconnect failed");
        uloop_timeout_set(&reconn_timer, 2000);
        return;
    }

    ubus_add_uloop(ctx);
    fcntl(ctx->sock.fd, F_SETFD, fcntl(ctx->sock.fd, F_GETFD) | FD_CLOEXEC);
}

static void ubus_conn_lost_cb(struct ubus_context *ctx)
{
    ump_info("connection lost");
    ubus_reconn_timer_cb(nullptr, ctx);
}

static void ubus_server_main(struct ubus_context *ctx)
{
    int ret = ubus_add_object(ctx, &control_obj);
    if (ret)
        ump_error("add object failed: %s", ubus_strerror(ret));
    uloop_run();
}

#ifdef ENABLE_ICEORYX
void onNV12Sample(iox::popo::Subscriber<NV12Frame> *subscriber)
{
    subscriber->take().and_then([](const auto &sample)
                                {
        const auto& frame = *sample;
        static int cnt = 0;
        if (++cnt % 100 == 0) {
            std::cout << "[iceoryx] NV12: " << frame.width << "x" << frame.height << std::endl;
        } })
        .or_else([](auto &result) {});
}

void onH264Sample(iox::popo::Subscriber<H264Frame> *subscriber)
{
    subscriber->take().and_then([](const auto &sample)
                                {
        const auto& frame = *sample;
        static int cnt = 0;
        if (++cnt % 100 == 0) {
            std::cout << "[iceoryx] H264: " << frame.size << " bytes" << std::endl;
        } })
        .or_else([](auto &result) {});
}
#endif

static void signal_handler(int sig)
{
    ump_info("signal %d, exiting", sig);
    uloop_end();
}

static void usage()
{
    printf("usage: video -f config_path\n");
}

static int create_pid_file()
{
    system("mkdir -p /var/run 2>/dev/null");

    int pid_fd = open(PID_FILE, O_CREAT | O_RDWR, 0644);
    if (pid_fd < 0)
    {
        printf("Failed to open pid file: %s\n", strerror(errno));
        return -1;
    }

    if (flock(pid_fd, LOCK_EX | LOCK_NB) < 0)
    {
        printf("Another instance is already running\n");
        close(pid_fd);
        return -1;
    }

    {
        std::ofstream pid_file(PID_FILE, std::ios::trunc);
        if (pid_file.is_open())
        {
            pid_file << getpid() << std::endl;
            pid_file.close();
        }
        else
        {
            printf("Failed to write pid via ofstream\n");
        }
    }

    return pid_fd;
}

static void remove_pid_file()
{
    if (g_pid_fd > 0)
    {
        flock(g_pid_fd, LOCK_UN);
        close(g_pid_fd);
        g_pid_fd = -1;
    }
    unlink(PID_FILE);
}

int main(int argc, char **argv)
{
    // ========== PID 文件检查 ==========
    g_pid_fd = create_pid_file();
    if (g_pid_fd < 0)
    {
        printf("Another instance is already running. Exiting.\n");
        return -1;
    }

    // 注册退出时清理 PID 文件
    atexit(remove_pid_file);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

#if defined(ENABLE_ROS2)
    rclcpp::init(argc, argv);
    auto subscriber = std::make_shared<ImageSubscriber>("image_subscriber_node");
    subscriber->subscribe_image(topics::CAMERA_IMAGE_JPEG,
                                [](const sensor_msgs::msg::CompressedImage::SharedPtr &msg) {});
    subscriber->subscribe_image(topics::CAMERA_IMAGE_H264,
                                [](const sensor_msgs::msg::CompressedImage::SharedPtr &msg) {});

    auto qr_subscriber = std::make_shared<StringSubscriber>("qr_subscriber_node");
    qr_subscriber->subscribe_string(topics::CAMERA_IMAGE_QR_RESULT,
                                    [](const std_msgs::msg::String::SharedPtr &msg) {});
    GlobalExecutor::getInstance().addNode(subscriber);
    GlobalExecutor::getInstance().addNode(qr_subscriber);
#endif

#ifdef ENABLE_ICEORYX
    iox::runtime::PoshRuntime::initRuntime("video");
    iox::popo::Subscriber<NV12Frame> nv12_subscriber({"NV12", "Image", "Frame"});
    iox::popo::Subscriber<H264Frame> h264_subscriber({"H264", "Video", "Stream"});
    iox::popo::Listener listener;
    listener.attachEvent(nv12_subscriber, iox::popo::SubscriberEvent::DATA_RECEIVED,
                         iox::popo::createNotificationCallback(onNV12Sample));
    listener.attachEvent(h264_subscriber, iox::popo::SubscriberEvent::DATA_RECEIVED,
                         iox::popo::createNotificationCallback(onH264Sample));
#endif

    if (LOG_INIT_SOK != log_module::init_zlog())
    {
        printf("init_zlog failed\r\n");
        return -1;
    }

    const char *cfgFile = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "f:v")) != -1)
    {
        switch (opt)
        {
        case 'f':
            cfgFile = optarg;
            break;
        case 'v':
            printf("1.0.0\n");
            return -1;
        default:
            usage();
            return -1;
        }
    }
    if (!cfgFile)
    {
        usage();
        return -1;
    }

    if (-1 == ecovacs::MediaEngine::MediaEngineInstance()->Init(cfgFile))
    {
        ump_error("mediaEngine init failed");
        return -1;
    }

    // ========== 订阅 NV12 视频帧 ==========
    MessageBus::instance().subscribe_async<ecovacs::NV12FrameMessage>("video/nv12_frame",
                                                                      [](const ecovacs::NV12FrameMessage &msg)
                                                                      {
                                                                          static uint64_t frame_count = 0;
                                                                          if (++frame_count % 100 == 0)
                                                                          {
                                                                              ump_info("NV12 frame #%llu: %dx%d", frame_count, msg.width, msg.height);
                                                                          }
                                                                      });

    // ========== ubus 初始化 ==========
    uloop_init();

    struct ubus_context *ctx = ubus_connect(ecovacs::MediaEngine::MediaEngineInstance()->media_engine_cfg_.unix_socket_path);
    if (!ctx)
    {
        ump_error("ubus connect failed");
        uloop_done();
        ecovacs::MediaEngine::MediaEngineInstance()->UnInit();
        log_module::uninit_zlog();
        return -1;
    }
    ctx->connection_lost = ubus_conn_lost_cb;
    ubus_add_uloop(ctx);
    fcntl(ctx->sock.fd, F_SETFD, fcntl(ctx->sock.fd, F_GETFD) | FD_CLOEXEC);

    ump_info("main loop started");
    ubus_server_main(ctx);

    // ========== 清理 ==========
    try
    {
        ump_info("Cleaning up...");

#if defined(ENABLE_ROS2)
        GlobalExecutor::getInstance().shutdown();
        rclcpp::shutdown();
#endif

        ubus_free(ctx);
        uloop_done();
        ecovacs::MediaEngine::MediaEngineInstance()->UnInit();
        log_module::uninit_zlog();
    }
    catch (const std::exception &e)
    {
        ump_error("Exception: %s", e.what());
    }

    ump_info("exit");

    return 0;
}
