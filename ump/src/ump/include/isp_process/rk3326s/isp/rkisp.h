#ifndef _RKISP_DEMO_H
#define _RKISP_DEMO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        int isp_fd;
        bool init_flag;
        unsigned int start_cnt;
        void *rkisp_engine;
    } IspCtxHandler;

    typedef struct tagManualInfo
    {
        double AeTime;
        double AeGain;
        double AeMaxTime;
        double AeMaxGain;
    } manual_info_t;

    manual_info_t set_3A_params(float gain, float expo);

    void stop_engine(void);

    void start_engine(void);

    void init_engine(void **_engine);

    void uninit_3A_params(void);

    void init_3A_params(void);

#ifdef __cplusplus
}
#endif

#endif //_RKISP_DEMO_H
