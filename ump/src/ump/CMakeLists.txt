
cmake_minimum_required(VERSION 3.0.2)
project(ump)

# ============================================================================
# 编译选项
# ============================================================================

# 根据构建类型设置优化选项
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# ============================================================================
# 全局编译选项（关键修改）
# ============================================================================
add_definitions(-DBOOST_STACKTRACE_USE_BACKTRACE)

# 基础编译标志
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -std=c++17 -fpermissive -Wno-psabi")

# ===== 关键修改：添加崩溃捕获所需的全局编译选项 =====
# 所有构建类型都保留帧指针和导出符号（用于完整的堆栈回溯）
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -funwind-tables -rdynamic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -funwind-tables -rdynamic")

# 链接器选项也需要 -rdynamic
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -rdynamic")

# Release 优化
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Os -DNDEBUG -ffunction-sections -fdata-sections")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Os -DNDEBUG -ffunction-sections -fdata-sections")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections -Wl,--as-needed")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--as-needed")
endif()

# Debug 选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
endif()

# ============================================================================
# 平台选项
# ============================================================================

option(USE_AW_PLATFORM "use aw platform flag" OFF)
option(USE_RK_PLATFORM "use rk platform flag" OFF)
option(USE_AM311X_PLATFORM "use am311x platform flag" OFF)
option(BUILD_TUTORIALS "Build tutorial examples" ON)

option(ENABLE_MP4_RECORD "enable mp4 video recording" OFF)
option(ENABLE_WEBRTC_TRANSPORT "enable webrtc transport module" OFF)


if(USE_RK_PLATFORM)
    add_definitions(-DRK_PLATFORM)
elseif(USE_AW_PLATFORM)
    add_definitions(-DAW_PLATFORM)
elseif(USE_AM311X_PLATFORM)
    add_definitions(-DAM311X_PLATFORM)
endif()

# ============================================================================
# 第三方库路径
# ============================================================================

set(THIRD_PARTY_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# WebRTC transport (official trimmed libwebrtc stack) — must be after THIRD_PARTY_BASE.
# NOTE: include path is applied target-privately below; do NOT use a global
# include_directories() here, otherwise the full WebRTC headers (which require
# C++20 and pollute the `rtc`/`webrtc` namespaces) leak into other targets
# such as audio_processor that rely on the separate webrtc-audio-processing
# headers and are compiled with C++17.
if(ENABLE_WEBRTC_TRANSPORT)
    set(WEBRTC_INSTALL ${THIRD_PARTY_BASE}/webrtc/install/root/usr)
    file(GLOB WEBRTC_RLIBS ${WEBRTC_INSTALL}/lib/webrtc_rlib/*.rlib)
endif()

# 通用第三方库
set(COMMON_THIRD_PARTY
    zlog
    libyuv
    libjpeg-turbo
    boost
    libbacktrace
)

foreach(lib ${COMMON_THIRD_PARTY})
    set(inc_dir ${THIRD_PARTY_BASE}/${lib}/install/root/usr/include)
    set(lib_dir ${THIRD_PARTY_BASE}/${lib}/install/root/usr/lib)
    if(EXISTS ${inc_dir})
        include_directories(${inc_dir})
    endif()
    if(EXISTS ${lib_dir})
        link_directories(${lib_dir})
    endif()
endforeach()

# ====== ALSA 库路径 ======
set(ALSA_ROOT ${THIRD_PARTY_BASE}/alsa-lib/install/root/usr)
if(EXISTS ${ALSA_ROOT})
    include_directories(${ALSA_ROOT}/include)
    link_directories(${ALSA_ROOT}/lib)
endif()

# ====== SpeexDSP 库路径 ======
set(SPEEXDSP_ROOT ${THIRD_PARTY_BASE}/speexdsp/install/root/usr)

if(EXISTS ${SPEEXDSP_ROOT})
    include_directories(${SPEEXDSP_ROOT}/include)
    link_directories(${SPEEXDSP_ROOT}/lib)
endif()

# ====== WebRTC 音频处理库路径 ======
set(WEBRTC_ROOT ${THIRD_PARTY_BASE}/webrtc-audio-processing/install/root/usr)
if(EXISTS ${WEBRTC_ROOT})
    set(WEBRTC_LIB ${WEBRTC_ROOT}/lib/libwebrtc-audio-processing-2.so)
endif()

# ====== MP4V2 录像库路径 ======
if(ENABLE_MP4_RECORD)
    set(MP4V2_ROOT ${THIRD_PARTY_BASE}/mp4v2/install/root/usr)
    if(EXISTS ${MP4V2_ROOT})
        include_directories(${MP4V2_ROOT}/include)
        link_directories(${MP4V2_ROOT}/lib)
    endif()
endif()

# ============================================================================
# Fast-DDS / Fast-CDR / foonathan_memory  依赖
# ============================================================================
set(fastrtps_DIR          ${THIRD_PARTY_BASE}/Fast-DDS/install/root/usr/share/fastrtps/cmake)
set(fastcdr_DIR           ${THIRD_PARTY_BASE}/Fast-CDR/install/root/usr/lib/cmake/fastcdr)
set(foonathan_memory_DIR  ${THIRD_PARTY_BASE}/foonathan_memory_vendor/install/root/usr/lib/foonathan_memory/cmake)
set(tinyxml2_DIR          ${THIRD_PARTY_BASE}/tinyxml2/install/root/usr/lib/cmake/tinyxml2)

# ============================================================================
# 项目头文件路径
# ============================================================================

set(PROJECT_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/chalk
    ${CMAKE_CURRENT_SOURCE_DIR}/include/logging
    ${CMAKE_CURRENT_SOURCE_DIR}/include/system_debug
    ${CMAKE_CURRENT_SOURCE_DIR}/include/os_platform
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ring_buffer
    ${CMAKE_CURRENT_SOURCE_DIR}/include/queue
    ${CMAKE_CURRENT_SOURCE_DIR}/include/message_bus
    ${CMAKE_CURRENT_SOURCE_DIR}/include/thread_pool
    ${CMAKE_CURRENT_SOURCE_DIR}/include/shm_fifo
    ${CMAKE_CURRENT_SOURCE_DIR}/include/video_common
    ${CMAKE_CURRENT_SOURCE_DIR}/include/video_capture
    ${CMAKE_CURRENT_SOURCE_DIR}/include/video_capture/rk_linux_v4l2
    ${CMAKE_CURRENT_SOURCE_DIR}/include/video_coding
    ${CMAKE_CURRENT_SOURCE_DIR}/include/audio_capture
    ${CMAKE_CURRENT_SOURCE_DIR}/include/audio_coding
    ${CMAKE_CURRENT_SOURCE_DIR}/include/audio_resample
    ${CMAKE_CURRENT_SOURCE_DIR}/include/audio_processor
    ${CMAKE_CURRENT_SOURCE_DIR}/include/webrtc_transport
)

if(ENABLE_MP4_RECORD)
    list(APPEND PROJECT_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include/video_recording)
endif()

foreach(dir ${PROJECT_INCLUDE_DIRS})
    include_directories(${dir})
endforeach()

# ============================================================================
# 平台特定路径
# ============================================================================

if(USE_RK_PLATFORM)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include/video_coding/rk_codec_impl)
    include_directories(${THIRD_PARTY_BASE}/mpp/install/root/usr/include/rockchip)
    include_directories(${THIRD_PARTY_BASE}/rockit/install/root/usr/include)
    include_directories(${THIRD_PARTY_BASE}/librga/install/root/usr/include/rga)
    
    link_directories(${THIRD_PARTY_BASE}/mpp/install/root/usr/lib)
    link_directories(${THIRD_PARTY_BASE}/rockit/install/root/usr/lib)
    link_directories(${THIRD_PARTY_BASE}/librga/install/root/usr/lib)
elseif(USE_AW_PLATFORM)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include/video_coding/allwinner_codec_impl)
    include_directories(${THIRD_PARTY_BASE}/libcedarc/install/root/usr/include)
    include_directories(${THIRD_PARTY_BASE}/libcedarc/install/root/usr/include/include)
    include_directories(${THIRD_PARTY_BASE}/libcedarc/install/root/usr/include/vencoder-base)
    
    link_directories(${THIRD_PARTY_BASE}/libcedarc/install/root/usr/lib)
elseif(USE_AM311X_PLATFORM)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include/video_capture/am311x_v4l2)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include/video_coding/am311x_codec_impl)
    # MHAL（Amicro 硬件抽象层）+ VPU（Hantro H264/JPEG 硬编，mhal 二级依赖）
    include_directories(${THIRD_PARTY_BASE}/mhal/install/root/usr/include/mhal)
    link_directories(${THIRD_PARTY_BASE}/mhal/install/root/usr/lib)
    link_directories(${THIRD_PARTY_BASE}/vpulib/install/root/usr/lib)
endif()

# ============================================================================
# 通用库
# ============================================================================
add_library(sysdbg_boost SHARED src/system_debug/sysdbg_boost.cpp)
target_link_libraries(sysdbg_boost backtrace dl)

add_library(log SHARED src/logging/log.cpp)
target_link_libraries(log zlog)
target_include_directories(log PRIVATE ${THIRD_PARTY_BASE}/zlog/install/root/usr/include)

add_library(osp SHARED src/os_platform/osp.cpp)

add_library(ringbuffer SHARED src/ring_buffer/ringbuffer.c)

add_library(chalk SHARED src/chalk/chalk.cpp)
target_link_libraries(chalk log)
add_dependencies(chalk log)

add_library(queue SHARED src/queue/queue.cpp)
target_link_libraries(queue osp)
add_dependencies(queue osp)

add_library(bus SHARED src/message_bus/bus.cpp)
target_link_libraries(bus queue osp)
add_dependencies(bus queue osp)

add_library(thread_pool SHARED src/thread_pool/msgqueue.c src/thread_pool/thrdpool.c)

add_library(shm_fifo SHARED src/shm_fifo/shmfifo.c)

add_library(video_common SHARED src/video_common/video_common_impl.cpp)
target_link_libraries(video_common osp)
add_dependencies(video_common osp)

# ===== ALSA 音频采集库 =====
add_library(audio_capture SHARED src/audio_capture/audio_capture.cpp)
target_link_libraries(audio_capture asound pthread)

# ===== G.711 音频编码库 =====
add_library(audio_coding SHARED
    src/audio_coding/audio_g711.c
    src/audio_coding/g711_encoder.cpp
)
target_include_directories(audio_coding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/audio_coding
)

# ===== 音频重采样库（SpeexDSP） =====
add_library(audio_resample SHARED src/audio_resample/resampler.cpp)
target_include_directories(audio_resample PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/audio_resample
)
target_link_libraries(audio_resample speexdsp)

# ===== 音频处理器（WebRTC APM） =====
add_library(audio_processor SHARED src/audio_processor/audio_processor.cpp)
target_include_directories(audio_processor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/audio_processor
    ${WEBRTC_ROOT}/include/webrtc-audio-processing-2
    ${WEBRTC_ROOT}/include
)
if(EXISTS ${WEBRTC_LIB})
    target_link_libraries(audio_processor ${WEBRTC_LIB})
else()
    target_link_libraries(audio_processor webrtc-audio-processing-2)
endif()

# ===== MP4 录像（mp4v2） =====
if(ENABLE_MP4_RECORD)
    add_library(video_recording SHARED src/video_recording/video_recorder.cpp)
    target_link_libraries(video_recording mp4v2)
endif()
# ===== WebRTC Transport（官方裁剪栈）=====
if(ENABLE_WEBRTC_TRANSPORT)
    add_library(webrtc_transport SHARED
        src/webrtc_transport/video.cpp
        src/webrtc_transport/audio.cpp
        src/webrtc_transport/peerconnection.cpp
        src/webrtc_transport/ws_signaling_client.cpp
    )
    target_include_directories(webrtc_transport PRIVATE
        ${WEBRTC_INSTALL}/include/webrtc
    )
    target_compile_definitions(webrtc_transport PRIVATE ENABLE_WEBRTC_TRANSPORT)
    set_source_files_properties(
        src/webrtc_transport/video.cpp
        src/webrtc_transport/audio.cpp
        src/webrtc_transport/peerconnection.cpp
        PROPERTIES COMPILE_FLAGS "-std=c++20 -fno-rtti -DNDEBUG -Wno-changes-meaning -Wno-deprecated-declarations -DWEBRTC_POSIX"
    )
    target_link_libraries(webrtc_transport
        log osp video_common
        -Wl,--start-group
        ${WEBRTC_INSTALL}/lib/libwebrtc.a
        ${WEBRTC_RLIBS}
        -lpthread -ldl -lm
        -Wl,--end-group
        -Wl,--gc-sections
    )
    message(STATUS "WebRTC Transport (trimmed, static): ON")
else()
    message(STATUS "WebRTC Transport (trimmed, static): OFF")

endif()
# ============================================================================
# 平台特定库
# ============================================================================

if(USE_RK_PLATFORM)
    add_library(video_capture SHARED 
        src/video_capture/device_info_impl.cpp
        src/video_capture/video_capture_impl.cpp
        src/video_capture/rk_linux_v4l2/video_capture_v4l2.cpp
        src/video_capture/rk_linux_v4l2/video_capture_linux.cpp
        src/video_capture/rk_linux_v4l2/device_info_v4l2.cpp
        src/video_capture/rk_linux_v4l2/device_info_linux.cpp
        src/video_capture/video_capture_factory.cpp)
    target_link_libraries(video_capture bus osp log video_common libyuv.so zlog turbojpeg)
    add_dependencies(video_capture bus osp log video_common)

    add_library(video_codec SHARED 
        src/video_coding/rk_codec_impl/rk_encoder_impl.cpp
        src/video_coding/rk_codec_impl/rk_encoder_handle.cpp
        src/video_coding/video_encoder_factory.cpp
        src/video_coding/video_encoder_impl.cpp)
    target_link_libraries(video_codec osp log rockit rockchip_mpp rga zlog)
    add_dependencies(video_codec log osp)
elseif(USE_AW_PLATFORM)
    add_library(video_capture SHARED 
        src/video_capture/device_info_impl.cpp
        src/video_capture/video_capture_impl.cpp
        src/video_capture/rk_linux_v4l2/video_capture_v4l2.cpp
        src/video_capture/rk_linux_v4l2/video_capture_linux.cpp
        src/video_capture/rk_linux_v4l2/device_info_v4l2.cpp
        src/video_capture/rk_linux_v4l2/device_info_linux.cpp
        src/video_capture/video_capture_factory.cpp)
    target_link_libraries(video_capture bus osp log video_common libyuv.so zlog turbojpeg)
    add_dependencies(video_capture bus osp log video_common)

    add_library(video_codec SHARED 
        src/video_coding/allwinner_codec_impl/allwinner_encoder_impl.cpp
        src/video_coding/allwinner_codec_impl/allwinner_encoder_handle.cpp
        src/video_coding/video_encoder_factory.cpp
        src/video_coding/video_encoder_impl.cpp)
    target_link_libraries(video_codec osp log zlog libMemAdapter.so libvencoder.so VE cdc_base venc_codec venc_base turbojpeg)
    add_dependencies(video_codec log osp)
elseif(USE_AM311X_PLATFORM)
    # ========== AM311x 采集（Media Controller + V4L2 + DMA-BUF） ==========
    add_library(video_capture SHARED 
        src/video_capture/device_info_impl.cpp
        src/video_capture/video_capture_impl.cpp
        src/video_capture/am311x_v4l2/video_capture_am311x.cpp
        src/video_capture/am311x_v4l2/video_capture_linux.cpp
        src/video_capture/am311x_v4l2/device_info_am311x.cpp
        src/video_capture/am311x_v4l2/device_info_linux.cpp
        src/video_capture/video_capture_factory.cpp)
    target_link_libraries(video_capture bus osp log video_common libyuv.so zlog turbojpeg)
    add_dependencies(video_capture bus osp log video_common)

    # ========== AM311x 编码（直接调 MHAL libmhal.so，流程移植自 camera_v2_venc.c） ==========
    add_library(video_codec SHARED 
        src/video_coding/am311x_codec_impl/am311x_encoder_impl.cpp
        src/video_coding/video_encoder_factory.cpp
        src/video_coding/video_encoder_impl.cpp)
    target_link_libraries(video_codec osp log zlog libmhal.so)
    add_dependencies(video_codec log osp)
endif()

# ============================================================================
# 示例程序
# ============================================================================

if(BUILD_TUTORIALS)
    add_subdirectory(src/tutorial/AudioExample)

    add_executable(tutorial_crash_test src/tutorial/tutorial_crash_test.cpp)
    target_link_libraries(tutorial_crash_test sysdbg_boost dl)

    if(USE_RK_PLATFORM OR USE_AW_PLATFORM OR USE_AM311X_PLATFORM)
        # ---- codec demo ----
        # video_codec 已各自 link 底层编码库（RK=libVE 等, AW=libvencoder, AM311x=libmhal）
        add_executable(tutorial_codec src/tutorial/tutorial_codec.cpp)
        target_link_libraries(tutorial_codec video_codec video_common dl)
        add_dependencies(tutorial_codec video_codec video_common)

        # ---- capture demo ----
        # video_capture link 了 libyuv，libyuv 二级依赖 libjpeg.so（标准 JPEG API）
        add_executable(tutorial_capture src/tutorial/tutorial_capture.cpp)
        target_link_libraries(tutorial_capture queue video_capture video_common
            osp dl log zlog libjpeg.so)
        add_dependencies(tutorial_capture queue video_capture video_common)

        # ---- audio demos ----
        add_executable(tutorial_audio_capture src/tutorial/tutorial_audio_capture.cpp)
        target_link_libraries(tutorial_audio_capture audio_capture audio_resample audio_processor)

        add_executable(tutorial_audio_g711_encoder src/tutorial/tutorial_audio_g711_encoder.cpp)
        target_link_libraries(tutorial_audio_g711_encoder audio_coding)
    endif()
endif()

# ============================================================================
# 安装
# ============================================================================

# 头文件
set(HEAD_FILES
    ${CMAKE_SOURCE_DIR}/include/os_platform/osp.h
    ${CMAKE_SOURCE_DIR}/include/chalk/chalk.h
    ${CMAKE_SOURCE_DIR}/include/ring_buffer/ringbuffer.h
    ${CMAKE_SOURCE_DIR}/include/queue/queue.h
    ${CMAKE_SOURCE_DIR}/include/system_debug/sysdbg.h
    ${CMAKE_SOURCE_DIR}/include/system_debug/sysdbg_boost.h
    ${CMAKE_SOURCE_DIR}/include/logging/log.h
    ${CMAKE_SOURCE_DIR}/include/message_bus/bus.h
    ${CMAKE_SOURCE_DIR}/include/shm_fifo/shmfifo.h
    ${CMAKE_SOURCE_DIR}/include/thread_pool/thrdpool.h
    ${CMAKE_SOURCE_DIR}/include/list/kernel_list.h
    ${CMAKE_SOURCE_DIR}/include/video_common/video_common_defines.h
    ${CMAKE_SOURCE_DIR}/include/video_capture/video_capture_defines.h
    ${CMAKE_SOURCE_DIR}/include/video_capture/video_capture.h
    ${CMAKE_SOURCE_DIR}/include/video_capture/video_capture_factory.h
    ${CMAKE_SOURCE_DIR}/include/video_coding/video_encoder.h
    ${CMAKE_SOURCE_DIR}/include/video_coding/video_encoder_factory.h
    ${CMAKE_SOURCE_DIR}/include/audio_capture/audio_capture.h
    ${CMAKE_SOURCE_DIR}/include/audio_coding/audio_g711.h
    ${CMAKE_SOURCE_DIR}/include/audio_coding/g711_encoder.h
    ${CMAKE_SOURCE_DIR}/include/audio_resample/resampler.h
    ${CMAKE_SOURCE_DIR}/include/audio_processor/audio_processor.h
)


if(ENABLE_MP4_RECORD)
    list(APPEND HEAD_FILES ${CMAKE_SOURCE_DIR}/include/video_recording/video_recorder.h)
endif()

if(ENABLE_WEBRTC_TRANSPORT)
    list(APPEND HEAD_FILES
        ${CMAKE_SOURCE_DIR}/include/webrtc_transport/webrtc_transport.h
        ${CMAKE_SOURCE_DIR}/include/webrtc_transport/ws_signaling_client.h
    )
endif()

INSTALL(FILES ${HEAD_FILES} DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/include/)

# 配置文件
INSTALL(DIRECTORY ${CMAKE_SOURCE_DIR}/conf
        DESTINATION ${CMAKE_INSTALL_PREFIX}/etc
        FILES_MATCHING PATTERN "*.conf")

# 动态库
set(SHARED_COMMON_LIB log osp ringbuffer queue bus thread_pool shm_fifo sysdbg_boost chalk)
if(USE_RK_PLATFORM OR USE_AW_PLATFORM OR USE_AM311X_PLATFORM)
    set(SHARED_MEDIA_LIB video_common video_capture video_codec)
endif()

# 安装所有库
set(INSTALL_LIBS ${SHARED_COMMON_LIB} ${SHARED_MEDIA_LIB} audio_capture audio_coding audio_resample audio_processor)

if(ENABLE_MP4_RECORD)
    list(APPEND INSTALL_LIBS video_recording)
endif()

if(ENABLE_WEBRTC_TRANSPORT)
    list(APPEND INSTALL_LIBS webrtc_transport)
endif()
INSTALL(TARGETS ${INSTALL_LIBS}
        LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/lib
        ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/lib
        RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/bin)

# 安装可执行文件
if(BUILD_TUTORIALS)
    INSTALL(TARGETS tutorial_crash_test tutorial_codec tutorial_capture tutorial_audio_capture tutorial_audio_g711_encoder
            RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/bin)
endif()

# ============================================================================
# 配置信息输出
# ============================================================================

message(STATUS "")
message(STATUS "UMP project configuration complete:")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
if(USE_RK_PLATFORM)
    message(STATUS "  Target: RK Platform")
elseif(USE_AW_PLATFORM)
    message(STATUS "  Target: Allwinner Platform")
elseif(USE_AM311X_PLATFORM)
    message(STATUS "  Target: AM311x Platform")
endif()
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build tutorials: ${BUILD_TUTORIALS}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
message(STATUS "Crash capture compile flags:")
message(STATUS "  CFLAGS: ${CMAKE_C_FLAGS}")
message(STATUS "  CXXFLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "  LDFLAGS: ${CMAKE_EXE_LINKER_FLAGS}")
message(STATUS "")
