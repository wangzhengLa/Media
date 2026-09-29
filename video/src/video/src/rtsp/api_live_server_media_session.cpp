// ============================================================================
// api_live_server_media_session.cpp
//
// 由 AX_Projec/rtsp/AXLiveServerMediaSession.cpp 适配。
// ============================================================================

#include "rtsp/api_live_server_media_session.h"
#include "H264VideoStreamFramer.hh"
#include "H264VideoRTPSink.hh"
#include "SimpleRTPSink.hh"
#include "GroupsockHelper.hh"

namespace apirtsp {

ApiLiveServerMediaSession* ApiLiveServerMediaSession::createNewVideo(
    UsageEnvironment& env, bool reuse_first_source, uint32_t n_max_frm_size) {
    return new ApiLiveServerMediaSession(env, reuse_first_source,
                                        true /*is_video*/, n_max_frm_size, 0, 0);
}

ApiLiveServerMediaSession* ApiLiveServerMediaSession::createNewAudio(
    UsageEnvironment& env, bool reuse_first_source,
    uint32_t n_sample_rate, uint8_t n_chn_cnt) {
    return new ApiLiveServerMediaSession(env, reuse_first_source,
                                        false /*is_video*/,
                                        API_RTSP_AUDIO_MAX_FRAME_SIZE,
                                        n_sample_rate, n_chn_cnt);
}

ApiLiveServerMediaSession::ApiLiveServerMediaSession(
    UsageEnvironment& env, bool reuse_first_source, bool is_video,
    uint32_t n_max_frm_size, uint32_t n_sample_rate, uint8_t n_chn_cnt)
    : OnDemandServerMediaSubsession(env, reuse_first_source),
      m_is_video(is_video),
      m_n_max_frm_buff_size(n_max_frm_size),
      m_n_sample_rate(n_sample_rate),
      m_n_chn_cnt(n_chn_cnt),
      f_aux_sdp_line_(NULL),
      f_done_flag_(0),
      f_dummy_sink_(NULL),
      m_p_source(NULL),
      m_p_i_frame_buf(nullptr),
      m_n_i_frame_chn(0),
      m_n_i_frame_size(0),
      m_n_i_frame_pts(0) {
    if (m_is_video) {
        m_p_i_frame_buf = new uint8_t[n_max_frm_size];
    }
}

ApiLiveServerMediaSession::~ApiLiveServerMediaSession(void) {
    delete[] f_aux_sdp_line_;
    if (m_p_i_frame_buf) {
        delete[] m_p_i_frame_buf;
        m_p_i_frame_buf = nullptr;
    }
}

void ApiLiveServerMediaSession::afterPlayingDummy(void* client_data) {
    ApiLiveServerMediaSession* session = (ApiLiveServerMediaSession*)client_data;
    session->AfterPlayingDummy();
}

void ApiLiveServerMediaSession::AfterPlayingDummy() {
    envir().taskScheduler().unscheduleDelayedTask(nextTask());
    SetDoneFlag();
}

void ApiLiveServerMediaSession::checkForAuxSDPLine(void* client_data) {
    ApiLiveServerMediaSession* session = (ApiLiveServerMediaSession*)client_data;
    session->CheckForAuxSDPLine();
}

void ApiLiveServerMediaSession::CheckForAuxSDPLine() {
    char const* dasl;
    if (f_aux_sdp_line_ != NULL) {
        SetDoneFlag();
    } else if (f_dummy_sink_ != NULL && (dasl = f_dummy_sink_->auxSDPLine()) != NULL) {
        f_aux_sdp_line_ = strDup(dasl);
        f_dummy_sink_ = NULL;
        SetDoneFlag();
    } else if (f_dummy_sink_ != NULL) {
        int u_secs_delay = 100000;  // 100ms
        nextTask() = envir().taskScheduler().scheduleDelayedTask(
            u_secs_delay, (TaskFunc*)checkForAuxSDPLine, this);
    }
}

char const* ApiLiveServerMediaSession::getAuxSDPLine(RTPSink* rtp_sink,
                                                    FramedSource* input_source) {
    if (!m_is_video) {
        // PCMA(SimpleRTPSink) 的 aux SDP 行在 sink 创建时即可生成，无需预读源
        return OnDemandServerMediaSubsession::getAuxSDPLine(rtp_sink, input_source);
    }

    if (f_aux_sdp_line_ != NULL) {
        return f_aux_sdp_line_;
    }

    // 视频：用 dummy sink 读源（首个数据为缓存的 SPS/PPS/IDR 整帧），
    // H264VideoRTPSink 从码流中解析出 SPS/PPS 后生成 sprop SDP 行
    if (f_dummy_sink_ == NULL) {
        f_done_flag_ = 0;
        f_dummy_sink_ = rtp_sink;
        f_dummy_sink_->startPlaying(*input_source, afterPlayingDummy, this);
        CheckForAuxSDPLine();
    }

    envir().taskScheduler().doEventLoop(&f_done_flag_);
    return f_aux_sdp_line_;
}

FramedSource* ApiLiveServerMediaSession::createNewStreamSource(
    unsigned /*client_session_id*/, unsigned& est_bitrate) {
    if (m_is_video) {
        est_bitrate = 8192;  // kbps
    } else {
        est_bitrate = 64;    // G711A: 8kHz * 8bit = 64kbps
    }

    ApiFramedSource* source = ApiFramedSource::createNew(envir(), m_n_max_frm_buff_size);

    std::lock_guard<std::mutex> lck(m_t_lock);
    m_p_source = source;

    if (m_is_video && m_p_i_frame_buf && m_n_i_frame_size > 0) {
        // 新客户端/新流：立即补发最近一个关键帧，避免起流花屏
        source->AddFrameBuff(m_n_i_frame_chn, m_p_i_frame_buf,
                             m_n_i_frame_size, m_n_i_frame_pts, true);
    }

    if (m_is_video) {
        // 输入为带 start code 的 Annex-B 字节流
        return H264VideoStreamFramer::createNew(envir(), source);
    } else {
        // G711 裸流，无需 Framer
        return source;
    }
}

void ApiLiveServerMediaSession::closeStreamSource(FramedSource* input_source) {
    if (input_source == NULL) {
        return;
    }
    std::lock_guard<std::mutex> lck(m_t_lock);
    // source 由 live555 随 framer/session 一起回收；先摘除指针，避免回收后跨线程访问
    m_p_source = NULL;
    Medium::close(input_source);
}

RTPSink* ApiLiveServerMediaSession::createNewRTPSink(
    Groupsock* rtp_groupsock, unsigned char rtp_payload_type_if_dynamic,
    FramedSource* /*input_source*/) {
    increaseSendBufferTo(envir(), rtp_groupsock->socketNum(), 500 * 1024);

    if (m_is_video) {
        // 与 ApiFramedSource::maxFrameSize() 对齐：大 IDR 帧不得被 OutPacketBuffer 截断
        OutPacketBuffer::increaseMaxSizeTo(API_RTSP_VIDEO_MAX_FRAME_SIZE);
        return H264VideoRTPSink::createNew(envir(), rtp_groupsock,
                                          rtp_payload_type_if_dynamic);
    } else {
        // PCMA 静态 payload type = 8，8kHz，单声道
        return SimpleRTPSink::createNew(envir(), rtp_groupsock,
                                        8 /*PCMA*/, m_n_sample_rate,
                                        "audio", "PCMA", m_n_chn_cnt,
                                        False /*allowMultipleFramesPerPacket*/);
    }
}

void ApiLiveServerMediaSession::SendNalu(uint8_t n_chn, const uint8_t* p_buf,
                                        uint32_t n_len, uint64_t n_pts /*= 0*/,
                                        bool b_i_frame /*= false*/) {
    std::lock_guard<std::mutex> lck(m_t_lock);

    if (b_i_frame && n_len > 0 && n_len <= m_n_max_frm_buff_size) {
        memcpy(m_p_i_frame_buf, p_buf, n_len);
        m_n_i_frame_size = n_len;
        m_n_i_frame_pts  = n_pts;
        m_n_i_frame_chn  = n_chn;
    }

    if (m_p_source) {
        m_p_source->AddFrameBuff(n_chn, p_buf, n_len, n_pts, b_i_frame);
    }
}

void ApiLiveServerMediaSession::SendAudio(uint32_t n_chn, const uint8_t* p_buf,
                                         uint32_t n_len, uint64_t n_pts) {
    std::lock_guard<std::mutex> lck(m_t_lock);
    if (m_p_source) {
        m_p_source->AddFrameBuff((uint8_t)n_chn, p_buf, n_len, n_pts, false);
    }
}

} // namespace apirtsp
