#ifndef VIDEO_CAPTURE_CONFIG_H_
#define VIDEO_CAPTURE_CONFIG_H_

namespace ecovacs
{
    namespace videocapturemodule
    {
        enum
        {
            kDefaultWidth = 640
        }; // Start width
        enum
        {
            kDefaultHeight = 480
        }; // Start heigt
        enum
        {
            kDefaultFrameRate = 30
        }; // Start frame rate

        enum
        {
            kMaxFrameRate = 60
        }; // Max allowed frame rate of the start image

        enum
        {
            kDefaultCaptureDelay = 120
        };
        enum
        {
            kMaxCaptureDelay = 270
        }; // Max capture delay allowed in the precompiled capture delay values.

        enum
        {
            kFrameRateCallbackInterval = 1000
        };
        enum
        {
            kFrameRateCountHistorySize = 90
        };
        enum
        {
            kFrameRateHistoryWindowMs = 2000
        };
    } // namespace videocapturemodule

} // namespace ecovacs

#endif // VIDEO_CAPTURE_CONFIG_H_
