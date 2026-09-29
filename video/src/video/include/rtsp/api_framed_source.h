#ifndef API_FRAMED_SOURCE_H_
#define API_FRAMED_SOURCE_H_

// ============================================================================
// api_framed_source.h
//
// 由 AX_Projec/rtsp/AXFramedSource.h 适配：
//   - live555 FramedSource 实现：生产者跨线程 AddFrameBuff 写入环形缓冲，
//     并通过 eventTrigger 唤醒 live555 事件循环线程取帧
// ============================================================================

#include <cstdint>
#include "FramedSource.hh"
#include "api_ring_buffer.h"

namespace apirtsp {

class ApiFramedSource : public FramedSource {
public:
    static ApiFramedSource* createNew(UsageEnvironment& env, uint32_t n_max_frm_size);

    // 生产者接口：把一帧（视频=带 start code 的 Annex-B 整帧；音频=G711 帧）拷入环形缓冲
    void AddFrameBuff(uint8_t n_chn, const uint8_t* p_buf, uint32_t n_len,
                      uint64_t n_pts = 0, bool b_i_frame = false);

    virtual unsigned maxFrameSize() const;

protected:
    ApiFramedSource(UsageEnvironment& env, uint32_t n_max_frm_size);
    virtual ~ApiFramedSource();

private:
    // redefined virtual functions:
    virtual void doGetNextFrame() override;

    static void deliverFrame(void* client_data);
    void DeliverFrame();

private:
    ApiRingBuffer* m_p_ring_buf;
    uint32_t      m_n_max_frm_size;   // 环形队列单槽位上限（1MB，仅约束入队帧大小）
    EventTriggerId m_n_trigger_id;

    // 分块投递状态：新版 live555 的 StreamParser 每次只按其 bank 容量(150000)读取，
    // 大访问单元(IDR)需切成多块连续投递，全部发完才从队列 Pop
    ApiRingElement* m_p_cur_element = nullptr;
    uint32_t       m_n_cur_offset = 0;
};

} // namespace apirtsp

#endif // API_FRAMED_SOURCE_H_
