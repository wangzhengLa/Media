#ifndef API_RING_BUFFER_H_
#define API_RING_BUFFER_H_

// ============================================================================
// api_ring_buffer.h
//
//   - 原有"关键帧感知"入队策略：
//       * 队列满时，新到关键帧可替换队尾（引用计数为 0 时），P 帧直接丢弃
//       * 一旦发生丢帧，后续 P 帧持续丢弃直到下一个关键帧（避免参考链断裂）
// ============================================================================

#include <cstdint>
#include <cstring>
#include <mutex>
#include "api_rtsp_common.h"

namespace apirtsp {

class ApiRingBuffer;

class ApiRingElement {
public:
    ApiRingElement()
        : n_index_(-1), p_buf_(nullptr), n_size_(0),
          n_channel_(0), b_i_frame_(false), n_pts_(0),
          n_ref_count_(0), p_parent_(nullptr) {}

    ApiRingElement(uint8_t* p_buf, uint32_t n_size, uint32_t n_chn,
                  uint64_t u64_pts = 0, bool is_i_frame = false)
        : n_index_(-1), p_buf_(p_buf), n_size_(n_size),
          n_channel_(n_chn), n_pts_(u64_pts), b_i_frame_(is_i_frame),
          n_ref_count_(0), p_parent_(nullptr) {}

    void CopyFrom(const ApiRingElement& element) {
        if (this->p_buf_ && element.p_buf_ && element.n_size_ > 0) {
            memcpy(this->p_buf_, element.p_buf_, element.n_size_);
        }
        this->n_size_ = element.n_size_;
        this->n_channel_ = element.n_channel_;
        this->n_pts_ = element.n_pts_;
        this->b_i_frame_ = element.b_i_frame_;

        this->n_ref_count_ = 0;
        this->p_parent_ = nullptr;
    }

    int32_t IncreaseRefCount() { return ++n_ref_count_; }

    int32_t DecreaseRefCount(bool b_force_clear) {
        if (b_force_clear) {
            n_ref_count_ = 0;
        } else {
            --n_ref_count_;
            if (n_ref_count_ < 0) {
                n_ref_count_ = 0;
            }
        }
        return n_ref_count_;
    }

    int32_t GetRefCount() const { return n_ref_count_; }
    int32_t GetIndex() const { return n_index_; }
    int32_t SetIndex(int32_t n_index) { return n_index_ = n_index; }

public:
    uint8_t*      p_buf_;
    uint32_t      n_size_;
    uint32_t      n_channel_;
    uint64_t      n_pts_;
    bool          b_i_frame_;
    ApiRingBuffer* p_parent_;

private:
    int32_t n_ref_count_;
    int32_t n_index_;
};

class ApiRingBuffer {
public:
    ApiRingBuffer(uint32_t n_element_buff_size, uint32_t n_element_count,
                 const char* psz_name = nullptr)
        : m_p_ring(nullptr), m_n_element_count(n_element_count),
          m_n_element_buff_size(n_element_buff_size),
          m_n_header(0), m_n_tail(0), m_b_has_lost(false) {
        m_sz_name[0] = '\0';
        if (psz_name && strlen(psz_name) > 0) {
            strncpy(m_sz_name, psz_name, sizeof(m_sz_name) - 1);
        }

        m_p_ring = new ApiRingElement[m_n_element_count];
        for (uint32_t i = 0; i < m_n_element_count; i++) {
            m_p_ring[i].p_buf_ = new uint8_t[m_n_element_buff_size];
            memset(m_p_ring[i].p_buf_, 0x0, m_n_element_buff_size);
            m_p_ring[i].n_size_ = 0;
            m_p_ring[i].p_parent_ = this;
            m_p_ring[i].SetIndex((int32_t)i);
        }
    }

    ~ApiRingBuffer() {
        std::lock_guard<std::mutex> lck(m_mutex);
        if (nullptr == m_p_ring) {
            return;
        }
        for (uint32_t i = 0; i < m_n_element_count; i++) {
            if (nullptr != m_p_ring[i].p_buf_) {
                delete[] m_p_ring[i].p_buf_;
            }
        }
        delete[] m_p_ring;
        m_p_ring = nullptr;
    }

    bool IsFull() {
        std::lock_guard<std::mutex> lck(m_mutex);
        return (m_n_tail - m_n_header) == m_n_element_count;
    }

    bool IsEmpty() {
        std::lock_guard<std::mutex> lck(m_mutex);
        return (m_n_tail == m_n_header);
    }

    bool Put(const ApiRingElement& element) {
        std::lock_guard<std::mutex> lck(m_mutex);

        if (!m_p_ring || element.n_size_ > m_n_element_buff_size) {
            // 帧超过单槽位大小：无法容纳
            API_RTSP_LOGW("[%s] drop frame: size=%u > slot=%u, iframe=%d",
                         m_sz_name, element.n_size_, m_n_element_buff_size,
                         element.b_i_frame_ ? 1 : 0);
            return false;
        }

        if ((m_n_tail - m_n_header) == m_n_element_count) {
            // 队列已满
            if (element.b_i_frame_) {
                // 用新关键帧替换队尾（队尾未被消费时）
                uint64_t n_index = (m_n_tail - 1) % m_n_element_count;
                int32_t n_ref_count = m_p_ring[n_index].GetRefCount();
                if (n_ref_count == 0) {
                    m_n_tail--;
                    m_b_has_lost = false;
                } else {
                    m_b_has_lost = true;
                    return false;
                }
            } else {
                // P 帧丢弃，标记失步：下一个关键帧之前的 P 帧全部不要
                m_b_has_lost = true;
                return false;
            }
        } else {
            if (element.b_i_frame_) {
                m_b_has_lost = false;  // 新关键帧恢复同步
            } else if (m_b_has_lost) {
                return false;
            }
        }

        uint64_t n_index = m_n_tail % m_n_element_count;
        if (m_p_ring[n_index].GetRefCount() != 0) {
            m_b_has_lost = true;
            return false;
        }

        m_p_ring[n_index].CopyFrom(element);
        m_p_ring[n_index].IncreaseRefCount();
        m_p_ring[n_index].p_parent_ = this;
        m_n_tail++;

        return true;
    }

    ApiRingElement* Get() {
        std::lock_guard<std::mutex> lck(m_mutex);
        if (m_n_header == m_n_tail) {
            return nullptr;
        }
        uint64_t n_index = m_n_header % m_n_element_count;
        m_p_ring[n_index].IncreaseRefCount();
        return &m_p_ring[n_index];
    }

    bool Pop(bool b_force = true) {
        std::lock_guard<std::mutex> lck(m_mutex);
        if (m_n_header == m_n_tail) {
            return false;
        }
        uint64_t n_index = m_n_header % m_n_element_count;
        m_p_ring[n_index].DecreaseRefCount(b_force);
        m_n_header++;
        return true;
    }

    void Free(ApiRingElement* ele, bool b_force = false) {
        if (!ele) {
            return;
        }
        std::lock_guard<std::mutex> lck(m_mutex);
        int32_t n_index = ele->GetIndex();
        if (n_index >= 0 && n_index < (int32_t)m_n_element_count) {
            m_p_ring[n_index].DecreaseRefCount(b_force);
        }
    }

    uint32_t Size() {
        if (m_n_tail < m_n_header) {
            return 0;
        }
        return (uint32_t)(m_n_tail - m_n_header);
    }

private:
    ApiRingElement* m_p_ring;
    uint32_t       m_n_element_count;
    uint32_t       m_n_element_buff_size;
    uint64_t       m_n_header;
    uint64_t       m_n_tail;
    bool           m_b_has_lost;
    std::mutex     m_mutex;
    char           m_sz_name[64];
};

} // namespace apirtsp

#endif // API_RING_BUFFER_H_
