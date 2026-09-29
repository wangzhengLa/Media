#ifndef API_LIVE_SERVER_MEDIA_SESSION_H_
#define API_LIVE_SERVER_MEDIA_SESSION_H_

// ============================================================================
// api_live_server_media_session.h
//
//       视频 H.264（Annex-B 带 start code，H264VideoStreamFramer 解析）
//       音频 G.711A / PCMA（8kHz 单声道，裸流 + SimpleRTPSink）
//   - std::mutex 替换 pthread_spinlock_t
//   - 缓存最近一个关键帧（SPS/PPS/IDR 整帧），新客户端建立时立即补发
// ============================================================================

#include <cstdint>
#include <mutex>
#include "liveMedia.hh"
#include "OnDemandServerMediaSubsession.hh"
#include "api_framed_source.h"

namespace apirtsp {

class ApiLiveServerMediaSession : public OnDemandServerMediaSubsession {
public:
    static ApiLiveServerMediaSession* createNewVideo(UsageEnvironment& env,
                                                    bool reuse_first_source,
                                                    uint32_t n_max_frm_size);
    static ApiLiveServerMediaSession* createNewAudio(UsageEnvironment& env,
                                                    bool reuse_first_source,
                                                    uint32_t n_sample_rate,
                                                    uint8_t n_chn_cnt);

    // 视频帧写入（Annex-B 整帧，关键帧带 SPS/PPS/IDR）
    void SendNalu(uint8_t n_chn, const uint8_t* p_buf, uint32_t n_len,
                  uint64_t n_pts = 0, bool b_i_frame = false);

    // 音频帧写入（G.711A 裸数据）
    void SendAudio(uint32_t n_chn, const uint8_t* p_buf, uint32_t n_len,
                   uint64_t n_pts);

protected:
    ApiLiveServerMediaSession(UsageEnvironment& env, bool reuse_first_source,
                             bool is_video, uint32_t n_max_frm_size,
                             uint32_t n_sample_rate, uint8_t n_chn_cnt);
    virtual ~ApiLiveServerMediaSession(void);

    // 新版 live555 中 EventLoopWatchVariable 为 std::atomic<char>（旧版为 volatile char）
    void SetDoneFlag() { f_done_flag_.store(1); }

protected:
    // redefined virtual functions:
    virtual char const* getAuxSDPLine(RTPSink* rtp_sink, FramedSource* input_source) override;
    virtual FramedSource* createNewStreamSource(unsigned client_session_id,
                                                unsigned& est_bitrate) override;
    virtual RTPSink* createNewRTPSink(Groupsock* rtp_groupsock,
                                      unsigned char rtp_payload_type_if_dynamic,
                                      FramedSource* input_source) override;
    virtual void closeStreamSource(FramedSource* input_source) override;

private:
    void CheckForAuxSDPLine();
    static void afterPlayingDummy(void* client_data);
    void AfterPlayingDummy();
    static void checkForAuxSDPLine(void* client_data);

private:
    bool           m_is_video;
    uint32_t       m_n_max_frm_buff_size;
    uint32_t       m_n_sample_rate;
    uint32_t       m_n_chn_cnt;

    char*          f_aux_sdp_line_;
    EventLoopWatchVariable f_done_flag_;
    RTPSink*       f_dummy_sink_;

    ApiFramedSource* m_p_source;
    std::mutex      m_t_lock;

    // 最近关键帧缓存（仅视频）
    uint8_t* m_p_i_frame_buf;
    uint8_t  m_n_i_frame_chn;
    uint32_t m_n_i_frame_size;
    uint64_t m_n_i_frame_pts;
};

} // namespace apirtsp

#endif // API_LIVE_SERVER_MEDIA_SESSION_H_
