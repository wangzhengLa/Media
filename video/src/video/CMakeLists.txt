cmake_minimum_required(VERSION 3.0.2)
project(video)

# ============================================================================
# 编译选项
# ============================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -rdynamic -fPIC -g -O0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -rdynamic -fPIC -g -O0 -std=c++17")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -rdynamic -fPIC -O2")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -rdynamic -fPIC -O2 -std=c++17")
endif()

# 屏蔽 ISP 相关的重复定义警告
add_definitions(-Wno-builtin-macro-redefined)
add_definitions(-Wno-redundant-decls)
# 添加编译选项，抑制 libubox 的警告
add_compile_options(-Wno-attributes)

# ============================================================================
# 平台和功能选项
# ============================================================================
option(USE_AW_PLATFORM "use aw platform flag" OFF)
option(USE_RK_PLATFORM "use rk platform flag" OFF)
option(ENABLE_ROS2 "enable ros2_humble flag" OFF)
option(ENABLE_WEBSOCKETS "enable websockets flag" OFF)
option(ENABLE_ICEORYX "enable iceoryx flag" OFF)
option(ENABLE_ISP "enable ISP management" OFF)
option(USE_LIVE555_RTSP "Use live555-based RTSP push (src/rtsp self-adapted wrapper) instead of xop" ON)
option(ENABLE_LIBPEER "enable libpeer WebRTC push" OFF)
option(ENABLE_WEBRTC "enable official transport-only trimmed WebRTC push" OFF)

# ============================================================================
# 平台特定定义
# ============================================================================
if(USE_RK_PLATFORM)
    add_definitions(-DRK_PLATFORM)
    set(PLATFORM_NAME "RK")
elseif(USE_AW_PLATFORM)
    add_definitions(-DAW_PLATFORM)
    set(PLATFORM_NAME "AW")
endif()

# ========== ISP 功能启用检查 ==========
# ISP 只在 RK 平台下才可能启用
if(ENABLE_ISP AND NOT USE_RK_PLATFORM)
    message(WARNING "ISP support is only available on RK platform! Disabling ISP.")
    set(ENABLE_ISP OFF)
endif()

# ISP 功能启用
if(ENABLE_ISP)
    add_definitions(-DENABLE_ISP)
    message(STATUS "ISP support enabled (RK Platform)")
else()
    message(STATUS "ISP support disabled")
endif()
# ======================================

# ============================================================================
# 第三方库路径配置
# ============================================================================
set(THIRD_PARTY_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../../)

set(THIRD_PARTY_LIBS
    # 基础库
    libbacktrace
    zlog
    cJSON
    libubox
    json-c
    ubus
    ump
    alsa-lib
    speexdsp
    webrtc-audio-processing
    # 媒体处理库
    libyuv
    libjpeg-turbo
    # 网络库
    libwebsockets
    rtspserver
    # 可选库
    boost
    iceoryx
)

# libpeer（WebRTC）默认不启用：开启时才配置其头文件/库路径
if(ENABLE_LIBPEER)
    list(APPEND THIRD_PARTY_LIBS libpeer)
endif()

# 官方裁剪栈 WebRTC 默认不启用（静态库自带 include 布局，单独配置）
if(ENABLE_WEBRTC)
    set(WEBRTC_INSTALL ${THIRD_PARTY_BASE}/webrtc/install/root/usr)
    include_directories(${WEBRTC_INSTALL}/include/webrtc)
    file(GLOB WEBRTC_RLIBS ${WEBRTC_INSTALL}/lib/webrtc_rlib/*.rlib)
endif()

if(USE_RK_PLATFORM)
    list(APPEND THIRD_PARTY_LIBS mpp rockit librga)
    # 只有启用 ISP 时才添加 camera_engine_rkaiq
    if(ENABLE_ISP)
        list(APPEND THIRD_PARTY_LIBS camera_engine_rkaiq)
    endif()
elseif(USE_AW_PLATFORM)
    # AW 平台特有库
    list(APPEND THIRD_PARTY_LIBS libcedarc libAWIspApi)
endif()

# ============================================================================
# 函数：配置第三方库
# ============================================================================
function(configure_third_party_library lib_name)
    set(include_dir ${THIRD_PARTY_BASE}/${lib_name}/install/root/usr/include)
    set(lib_dir ${THIRD_PARTY_BASE}/${lib_name}/install/root/usr/lib)
    if(EXISTS ${include_dir})
        include_directories(${include_dir})
        message(DEBUG "Added include for ${lib_name}: ${include_dir}")
    else()
        message(WARNING "Include dir for ${lib_name} not found: ${include_dir}")
    endif()
    
    if(EXISTS ${lib_dir})
        link_directories(${lib_dir})
        message(DEBUG "Added library dir for ${lib_name}: ${lib_dir}")
    else()
        message(WARNING "Library dir for ${lib_name} not found: ${lib_dir}")
    endif()
endfunction()

foreach(lib ${THIRD_PARTY_LIBS})
    configure_third_party_library(${lib})
endforeach()


# AW 平台特殊处理
if(USE_AW_PLATFORM)
    include_directories(${THIRD_PARTY_BASE}/libcedarc/install/root/usr/include/include)
    include_directories(${THIRD_PARTY_BASE}/libcedarc/install/root/usr/include/vencoder-base)
endif()

# Boost 特殊处理
include_directories(${THIRD_PARTY_BASE}/boost/src/boost-1.80.0)


# ============================================================================
# 项目包含路径
# ============================================================================
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include/ros2)

# RK AIQ 头文件（仅当 ISP 启用且 RK 平台时）
if(ENABLE_ISP AND USE_RK_PLATFORM)
    set(RKAIQ_INCLUDE_DIRS
        rkaiq
        rkaiq/algos
        rkaiq/uAPI2
        rkaiq/common
        rkaiq/iq_parser
        rkaiq/iq_parser_v2
        rkaiq/smartir
        rkaiq/xcore
    )
    foreach(dir ${RKAIQ_INCLUDE_DIRS})
        include_directories(${THIRD_PARTY_BASE}/camera_engine_rkaiq/install/root/usr/include/${dir})
    endforeach()
endif()

# ============================================================================
# ROS2 配置
# ============================================================================
if(ENABLE_ROS2)
    add_definitions(-DENABLE_ROS2)
    
    set(ROS2_INCLUDE_DIRS
        attr/include
        acl/include
        ros2-humble/include
        ros2-humble/include/builtin_interfaces
        ros2-humble/include/libstatistics_collector
        ros2-humble/include/rclcpp
        ros2-humble/include/rcl
        ros2-humble/include/rcutils
        ros2-humble/include/rmw
        ros2-humble/include/rcl_interfaces
        ros2-humble/include/rcl_yaml_param_parser
        ros2-humble/include/rosidl_runtime_c
        ros2-humble/include/rosidl_runtime_cpp
        ros2-humble/include/rosidl_typesupport_interface
        ros2-humble/include/rcpputils
        ros2-humble/include/statistics_msgs
        ros2-humble/include/tracetools
        ros2-humble/include/example_interfaces
        ros2-humble/include/sensor_msgs
        ros2-humble/include/std_msgs
    )
    
    foreach(dir ${ROS2_INCLUDE_DIRS})
        include_directories(${THIRD_PARTY_BASE}/${dir})
    endforeach()
    
    link_directories(${THIRD_PARTY_BASE}/attr/lib)
    link_directories(${THIRD_PARTY_BASE}/acl/lib)
    link_directories(${THIRD_PARTY_BASE}/ros2-humble/lib)
    
    set(ROS2_LIBS
        # 核心库
        rclcpp rcl rcutils rcpputils rmw yaml
        # 消息支持
        rosidl_runtime_c rosidl_typesupport_c rosidl_typesupport_cpp
        rosidl_typesupport_introspection_c rosidl_typesupport_introspection_cpp
        # 接口支持
        action_msgs__rosidl_generator_c action_msgs__rosidl_typesupport_c
        action_msgs__rosidl_typesupport_cpp action_msgs__rosidl_typesupport_introspection_c
        action_msgs__rosidl_typesupport_introspection_cpp
        rcl_interfaces__rosidl_generator_c rcl_interfaces__rosidl_typesupport_c
        rcl_interfaces__rosidl_typesupport_cpp rcl_interfaces__rosidl_typesupport_introspection_c
        rcl_interfaces__rosidl_typesupport_introspection_cpp
        # 日志系统
        rcl_logging_spdlog rcl_logging_interface spdlog
        # DDS相关
        ddsc rmw_cyclonedds_cpp rmw_dds_common
        rmw_dds_common__rosidl_typesupport_cpp rmw_dds_common__rosidl_typesupport_introspection_cpp
        # 统计与跟踪
        libstatistics_collector tracetools
        # 其他
        ament_index_cpp rcl_yaml_param_parser
        iceoryx_binding_c iceoryx_hoofs iceoryx_posh iceoryx_platform
        # 消息类型
        builtin_interfaces__rosidl_generator_c builtin_interfaces__rosidl_typesupport_c
        builtin_interfaces__rosidl_typesupport_cpp builtin_interfaces__rosidl_typesupport_introspection_c
        builtin_interfaces__rosidl_typesupport_introspection_cpp
        example_interfaces__rosidl_generator_c example_interfaces__rosidl_typesupport_c
        example_interfaces__rosidl_typesupport_cpp example_interfaces__rosidl_typesupport_introspection_c
        example_interfaces__rosidl_typesupport_introspection_cpp
        rosgraph_msgs__rosidl_generator_c rosgraph_msgs__rosidl_typesupport_c
        rosgraph_msgs__rosidl_typesupport_cpp rosgraph_msgs__rosidl_typesupport_introspection_c
        rosgraph_msgs__rosidl_typesupport_introspection_cpp
        statistics_msgs__rosidl_typesupport_c statistics_msgs__rosidl_typesupport_cpp
        statistics_msgs__rosidl_typesupport_introspection_c statistics_msgs__rosidl_generator_c
        statistics_msgs__rosidl_typesupport_introspection_cpp
        unique_identifier_msgs__rosidl_generator_c unique_identifier_msgs__rosidl_typesupport_c
        unique_identifier_msgs__rosidl_typesupport_cpp unique_identifier_msgs__rosidl_typesupport_introspection_c
        unique_identifier_msgs__rosidl_typesupport_introspection_cpp
        std_msgs__rosidl_generator_c std_msgs__rosidl_typesupport_c std_msgs__rosidl_typesupport_cpp
        std_msgs__rosidl_typesupport_introspection_c std_msgs__rosidl_typesupport_introspection_cpp
        geometry_msgs__rosidl_generator_c geometry_msgs__rosidl_typesupport_c geometry_msgs__rosidl_typesupport_cpp
        geometry_msgs__rosidl_typesupport_introspection_c geometry_msgs__rosidl_typesupport_introspection_cpp
        sensor_msgs__rosidl_generator_c sensor_msgs__rosidl_typesupport_c sensor_msgs__rosidl_typesupport_cpp
        sensor_msgs__rosidl_typesupport_introspection_c sensor_msgs__rosidl_typesupport_introspection_cpp
        acl attr
    )
endif()

# ============================================================================
# 可选功能配置
# ============================================================================
# WebSocket 支持
if(ENABLE_WEBSOCKETS)
    add_definitions(-DENABLE_WEBSOCKETS)
    set(WEBSOCKET_SOURCES 
        src/api_soft_jpeg_encoder.cpp
        src/api_websocket_server.cpp
    )
    set(WEBSOCKET_LIBS libwebsockets.so libturbojpeg.so)
    message(STATUS "WebSocket support: ON")
else()
    set(WEBSOCKET_SOURCES)
    set(WEBSOCKET_LIBS)
    message(STATUS "WebSocket support: OFF")
endif()

# Iceoryx 支持
if(ENABLE_ICEORYX)
    add_definitions(-DENABLE_ICEORYX)
    set(ICEORYX_LIBS libiceoryx_posh.so libiceoryx_hoofs.so libiceoryx_platform.so)
    message(STATUS "Iceoryx support: ON")
else()
    set(ICEORYX_LIBS)
    message(STATUS "Iceoryx support: OFF")
endif()


# Libpeer（WebRTC）支持：默认关闭，开启时编译 libpeer push 与信令客户端
if(ENABLE_LIBPEER)
    add_definitions(-DENABLE_LIBPEER)
    set(LIBPEER_SOURCES
        src/api_video_libpeer_push.cpp
        src/signaling_client.c
    )
    set(LIBPEER_LIBS libpeer.so)
    message(STATUS "Libpeer support: ON (WebRTC)")
else()
    set(LIBPEER_SOURCES)
    set(LIBPEER_LIBS)
    message(STATUS "Libpeer support: OFF")
endif()

# WebRTC push now backed by ump's webrtc_transport component (which links
# libwebrtc.a internally). video only needs the thin wrapper + ump lib.
if(ENABLE_WEBRTC)
    add_definitions(-DENABLE_WEBRTC)
    set(WEBRTC_SOURCES
        src/api_video_webrtc_push.cpp
    )
    set(WEBRTC_LIBS webrtc_transport)
    message(STATUS "Official WebRTC (via ump webrtc_transport): ON")
else()
    set(WEBRTC_SOURCES)
    set(WEBRTC_LIBS)
    message(STATUS "Official WebRTC (via ump webrtc_transport): OFF")
endif()

# ============================================================================
# 源文件列表
# ============================================================================
set(VIDEO_SOURCES
    # 核心模块
    src/api_video_capture.cpp
    src/api_video_capture_broadcaster.cpp
    src/api_video_encode.cpp
    src/api_video_encode_broadcaster.cpp
    src/api_video_push.cpp
    src/api_video_ai_process.cpp
    src/api_media_engine.cpp
    src/api_message_dispatcher.cpp
    src/api_allwinner_isp.cpp
    # 辅助模块
    src/api_jpeg_upload.cpp
    src/api_raw_video_save.cpp
    src/api_jpeg_publish.cpp
    src/inproc_comm.cpp
    # 主程序
    src/main.cpp
)

# ============================================================================
# RTSP 推流后端选择：live555（自有封装：include/rtsp + src/rtsp）/ xop
# ============================================================================
if(USE_LIVE555_RTSP)
    # ---- live555 实现：include/rtsp + src/rtsp（由 AX_Projec/rtsp 适配的自有封装）----
    # NO_OPENSSL 必须跟 live555 库编译时一致，否则 TLSState.hh 会强依赖 openssl 头
    add_definitions(-DUSE_LIVE555_RTSP -DNO_OPENSSL)

    set(LIVE555_INSTALL ${THIRD_PARTY_BASE}/live555/install/root/usr)

    # 头文件路径：install 后所有 .hh 合并到 liveMedia/ 子目录
    include_directories(
        ${LIVE555_INSTALL}/include/liveMedia    # #include "BasicUsageEnvironment.hh" 形式
    )
    # 自有 RTSP 封装头文件在 include/rtsp（include/ 已在上方全局 include_directories 中，
    # 代码内以 #include "rtsp/api_xxx.h" 引用）

    # 库搜索路径
    link_directories(${LIVE555_INSTALL}/lib)

    list(APPEND VIDEO_SOURCES
        src/rtsp/api_framed_source.cpp
        src/rtsp/api_live_server_media_session.cpp
        src/rtsp/api_rtsp_server.cpp
        src/api_video_live555_push.cpp
    )

    message(STATUS "RTSP backend: live555 (self-adapted wrapper in include/rtsp + src/rtsp, prebuilt from ${LIVE555_INSTALL})")
else()
    # ---- xop 实现（原方案）----
    list(APPEND VIDEO_SOURCES src/api_video_rtsp_push.cpp)
    message(STATUS "RTSP backend: xop (USE_LIVE555_RTSP=OFF)")
endif()

# 可选模块
if(ENABLE_ISP)
    list(APPEND VIDEO_SOURCES src/api_rk_isp_manager.cpp)
    message(STATUS "ISP module: included")
endif()

if(ENABLE_WEBSOCKETS)
    list(APPEND VIDEO_SOURCES ${WEBSOCKET_SOURCES})
endif()

if(ENABLE_LIBPEER)
    list(APPEND VIDEO_SOURCES ${LIBPEER_SOURCES})
endif()

if(ENABLE_WEBRTC)
    list(APPEND VIDEO_SOURCES ${WEBRTC_SOURCES})
endif()

# ============================================================================
# 链接库列表
# ============================================================================
set(BASE_LIBS
    libbus.so
    libqueue.so
    libcjson.so
    libosp.so
    liblog.so
    libvideo_common.so
    libvideo_codec.so
    libvideo_capture.so
    libjson-c.so
    libubox.so
    libblobmsg_json.so
    libubus.so
    libzlog.so
    libyuv.so
    libm.so
    libdl.so
    libpthread.so
    libturbojpeg.so
    libjpeg.so
    libbacktrace.so
    sysdbg_boost.so
    libasound.so
    libspeexdsp.so
    libaudio_capture.so
    libaudio_coding.so
    libaudio_resample.so
    libaudio_processor.so
    libwebrtc-audio-processing-2.so
    libringbuffer.so
)

if(USE_AW_PLATFORM)
   set(AW_ISP_LIBS libAWIspApi.so libiniparser.so libisp_af.so libisp_awb.so libisp_gtm.so libisp_iso.so libisp_math.so libisp_pltm.so libisp.so
		   libisp_ae.so libisp_afs.so libisp_base.so libisp_dev.so libisp_ini.so libisp_md.so libisp_rolloff.so)	
   list(APPEND BASE_LIBS libMemAdapter.so libvencoder.so libVE.so libcdc_base.so libvenc_codec.so libvenc_base.so ${AW_ISP_LIBS})
 endif()

# 可选库
set(OPTIONAL_LIBS
    ${WEBSOCKET_LIBS}
    ${ICEORYX_LIBS}
)
# 平台特定库
if(USE_RK_PLATFORM)
    set(RK_LIBS librockchip_mpp.so librockit.so librga.so)
    if(ENABLE_ISP)
        list(APPEND RK_LIBS librkaiq.so)
    endif()
endif()

# ROS2 库
if(ENABLE_ROS2)
    set(ROS2_LIBS_FINAL ${ROS2_LIBS})
endif()

# ============================================================================
# RTSP 后端条件库
# ============================================================================
if(USE_LIVE555_RTSP)
    # live555 预编译静态库（4 个 .a 有循环依赖，用 start-group 包裹）
    set(RTSP_BACKEND_LIBS
        -Wl,--start-group
        liveMedia.a
        BasicUsageEnvironment.a
        UsageEnvironment.a
        groupsock.a
        -Wl,--end-group
        rt              # live555 BasicTaskScheduler 定时器
        resolv          # live555 DNS 解析
    )
else()
    # xop 动态库
    set(RTSP_BACKEND_LIBS
        librtsp_server.so
    )
endif()

# 合并所有库
set(ALL_LIBS
    ${BASE_LIBS}
    ${OPTIONAL_LIBS}
    ${RK_LIBS}
    ${ROS2_LIBS_FINAL}
    ${RTSP_BACKEND_LIBS}
    ${LIBPEER_LIBS}
    ${WEBRTC_LIBS}
)

# ============================================================================
# 构建目标
# ============================================================================
add_executable(video ${VIDEO_SOURCES})

foreach(lib ${ALL_LIBS})
    target_link_libraries(video ${lib})
endforeach()

# ============================================================================
# 安装配置
# ============================================================================
# 配置文件
install(DIRECTORY ${CMAKE_SOURCE_DIR}/conf
    DESTINATION ${CMAKE_INSTALL_PREFIX}/etc
    FILES_MATCHING 
    PATTERN "*.json"
    PATTERN "*.toml"
)

# 脚本文件
set(INSTALL_SCRIPTS
    video.sh
    video_hook.sh
)
foreach(script ${INSTALL_SCRIPTS})
    install(FILES scripts/${script}
        DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/bin
        PERMISSIONS 
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
        RENAME ${script}
    )
endforeach()

# ROS2 库安装
if(ENABLE_ROS2)
    install(DIRECTORY ${THIRD_PARTY_BASE}/ros2-humble/lib/
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        FILES_MATCHING 
        PATTERN "*.so*"
        PATTERN "action_tutorials_cpp" EXCLUDE
        PATTERN "console_bridge" EXCLUDE
        PATTERN "cmake" EXCLUDE
        PATTERN "demo_nodes_cpp" EXCLUDE
        PATTERN "pkgconfig" EXCLUDE
        PATTERN "python3.10" EXCLUDE
        PATTERN "rclcpp_components" EXCLUDE
        PATTERN "rosidl_*" EXCLUDE
        PATTERN "tracetools" EXCLUDE
    )
endif()

# 可执行文件
install(TARGETS video
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/lib
    ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/lib
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/bin
)

# ============================================================================
# 配置总结
# ============================================================================
message(STATUS "")
message(STATUS "==================== Video Project Configuration ====================")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Platform:       ${PLATFORM_NAME}")
message(STATUS "  C compiler:     ${CMAKE_C_COMPILER}")
message(STATUS "  C++ compiler:   ${CMAKE_CXX_COMPILER}")
message(STATUS "")
message(STATUS "  Features:")
message(STATUS "    ISP:          ${ENABLE_ISP}")
message(STATUS "    WebSocket:    ${ENABLE_WEBSOCKETS}")
message(STATUS "    Iceoryx:      ${ENABLE_ICEORYX}")
message(STATUS "    Libpeer:      ${ENABLE_LIBPEER}")
message(STATUS "    WebRTC:       ${ENABLE_WEBRTC}")
message(STATUS "    ROS2:         ${ENABLE_ROS2}")
message(STATUS "")
message(STATUS "  Source files:   ${VIDEO_SOURCES}")
message(STATUS "  Link libraries: ${ALL_LIBS}")
message(STATUS "====================================================================")
message(STATUS "")
