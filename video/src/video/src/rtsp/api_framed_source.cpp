// ============================================================================
// api_framed_source.cpp

// ============================================================================

#include "rtsp/api_framed_source.h"
#include "GroupsockHelper.hh"

namespace apirtsp {

ApiFramedSource* ApiFramedSource::createNew(UsageEnvironment& env, uint32_t n_max_frm_size) {
    return new ApiFramedSource(env, n_max_frm_size);
}

ApiFramedSource::ApiFramedSource(UsageEnvironment& env, uint32_t n_max_frm_size)
    : FramedSource(env),
      m_p_ring_buf(nullptr),
      m_n_max_frm_size(n_max_frm_size),
      m_n_trigger_id(0) {
    m_n_trigger_id = envir().taskScheduler().createEventTrigger(deliverFrame);
    m_p_ring_buf = new ApiRingBuffer(n_max_frm_size, API_RTSP_RING_ELEMENT_COUNT, "RTSP");
}

ApiFramedSource::~ApiFramedSource() {
    envir().taskScheduler().deleteEventTrigger(m_n_trigger_id);
    m_n_trigger_id = 0;
    delete m_p_ring_buf;
    m_p_ring_buf = nullptr;
}

void ApiFramedSource::AddFrameBuff(uint8_t n_chn, const uint8_t* p_buf, uint32_t n_len,
                                  uint64_t n_pts /*= 0*/, bool b_i_frame /*= false*/) {
    ApiRingElement ele(const_cast<uint8_t*>(p_buf), n_len, n_chn, n_pts, b_i_frame);
    m_p_ring_buf->Put(ele);

    // 跨线程唤醒 live555 事件循环
    envir().taskScheduler().triggerEvent(m_n_trigger_id, this);
}

void ApiFramedSource::doGetNextFrame() {
    DeliverFrame();
}

void ApiFramedSource::deliverFrame(void* client_data) {
    ((ApiFramedSource*)client_data)->DeliverFrame();
}

void ApiFramedSource::DeliverFrame() {
    if (!isCurrentlyAwaitingData()) {
        return;  // live555 还没准备好接收数据
    }

    // 上一块已发完：取新的访问单元；否则续发当前单元的剩余部分
    if (m_p_cur_element == nullptr) {
        m_p_cur_element = m_p_ring_buf->Get();
        if (!m_p_cur_element) {
            return;  // 环形缓冲为空，等待下一次 triggerEvent 唤醒
        }
        m_n_cur_offset = 0;
    }

    ApiRingElement* element = m_p_cur_element;
    uint8_t* p_frame_data = element->p_buf_;
    uint32_t n_frame_size = element->n_size_;
    if (n_frame_size == 0 || nullptr == p_frame_data) {
        m_p_ring_buf->Pop();
        m_p_cur_element = nullptr;
        return;
    }

    // 按下游本次给的 buffer(fMaxSize) 切块：
    //   - H264VideoStreamFramer 的 StreamParser bank 只有 150000 字节
    //   - SimpleRTPSink(音频) buffer 远大于单音频帧
    // 切出的只是连续字节流的一块，parser 靠 start code 自行分帧
    uint32_t n_remaining = n_frame_size - m_n_cur_offset;
    uint32_t n_chunk = (fMaxSize < n_remaining) ? fMaxSize : n_remaining;

    fFrameSize = n_chunk;
    fNumTruncatedBytes = 0;   // 分块是正常流式语义，不是截断丢数据
    // 使用生产端记录的绝对时间戳（微秒 → timeval），live555 据此生成 RTP 时间戳
    fPresentationTime.tv_sec  = (time_t)(element->n_pts_ / 1000000ULL);
    fPresentationTime.tv_usec = (long)(element->n_pts_ % 1000000ULL);

    memmove(fTo, p_frame_data + m_n_cur_offset, n_chunk);
    m_n_cur_offset += n_chunk;

    // 整个访问单元发完才真正出队，期间槽位引用计数保护其不被生产者覆盖
    if (m_n_cur_offset >= n_frame_size) {
        m_p_ring_buf->Pop();
        m_p_cur_element = nullptr;
    }

    // 通知读取者数据已可用
    FramedSource::afterGetting(this);
}

unsigned ApiFramedSource::maxFrameSize() const {

    return 0;
}

} // namespace apirtsp
