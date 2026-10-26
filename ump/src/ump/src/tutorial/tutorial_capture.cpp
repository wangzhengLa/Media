#include <signal.h>
#include <iostream>
#include <cstring>
#include <memory>
#include "osp.h"
#include "log.h"
#include "video_capture_factory.h"

using namespace ecovacs;

class CaptureSink : public video_common::VideoSinkInterface
{
public:
	CaptureSink()
	{
		nv12_fp_ = fopen("/userdata/640*480.nv12", "wb+");
	}
	~CaptureSink()
	{
		if (nv12_fp_)
			fclose(nv12_fp_);
	}

	void OnFrame(video_common::VideoFrameBuffer *frame)
	{
		if (nv12_fp_)
		{
			ump_info("capture one freame\r\n");
		}
	}

private:
	FILE *nv12_fp_;
};

static bool g_running = true;

static void signal_handler(int signal)
{
	std::cout << "---> signal = " << signal << " <----" << std::endl;
	if (signal == SIGTSTP || signal == SIGINT)
	{
		g_running = false;
		exit(0);
	}
}

int main(int argc, char **argv)
{
	/* 信号处理 */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, &signal_handler);
	signal(SIGTSTP, &signal_handler);

	log_module::init_zlog();

	ump_info("log init finish\n");

	size_t width = 640;
	size_t height = 480;
	size_t target_fps = 30;
	size_t device_index = 0;

	// 1、创建捕获视频采集设备探测模块
	std::unique_ptr<VideoCaptureModule::DeviceInfo>
		device_info(VideoCaptureFactory::CreateDeviceInfo());

	ump_info("width[%d] height[%d] target_fps[%d] device_index[%d]\r\n", width, height, target_fps, device_index);

	// 2、探测可用的视频采集设备数
	uint32_t number = device_info->NumberOfDevices();
	ump_info("video caputre devices number[%d]\r\n", number);
	if (0 == number)
	{
		ump_info("no video device can use\r\n");
		return -1;
	}

	// 3、获取应用层指定索引号的设备device_name和unique_name
	char device_name[256] = {0};
	char unique_name[256] = {0};
	if (device_info->GetDeviceName(static_cast<uint32_t>(device_index),
								   device_name, sizeof(device_name),
								   unique_name, sizeof(unique_name)) != 0)
	{
		ump_error("GetDevideName device_index[%d] failed\r\n", device_index);
		return -1;
	}

	ump_info("device_index[%d] device_name[%s] unique_name[%s]\r\n", device_index, device_name, unique_name);

	// 4、查看当前设备支持的能力集数量
	int32_t capNum = device_info->NumberOfCapabilities(unique_name);
	ump_info("cap num [%d]\r\n", capNum);
	if (0 == capNum)
	{
		ump_error("no Capabilities can use\r\n");
		return -1;
	}

	// 5、请求能力集匹配到设备的最佳能力集
	VideoCaptureCapability requestCap;
	requestCap.width = static_cast<int32_t>(width);
	requestCap.height = static_cast<int32_t>(height);
	requestCap.maxFPS = static_cast<int32_t>(target_fps);
	requestCap.videoType = video_common::VideoType::kNV12;

	VideoCaptureCapability bestCap = requestCap;
	int32_t num = device_info->GetBestMatchedCapability(unique_name, requestCap, bestCap);
	ump_info("best match num[%d]\r\n", num);
	if (0 == num)
	{
		ump_error("no MatchedCapability can use\r\n");
		return -1;
	}

	VideoCaptureCapability capability_ = bestCap;
	capability_.maxFPS = requestCap.maxFPS; // rk芯片不支持帧率设置，这里使用时首先探测好，写为真实的采集帧率,x3目前也是固定帧率fps=30

	ump_info("bestCap width[%d] height[%d] maxFPS[%d] videoType[%d]\r\n", bestCap.width, bestCap.height,
			 bestCap.maxFPS, bestCap.videoType);

	// 6、创建视频采集设备模块
	VideoCaptureModule *vcm_ = VideoCaptureFactory::Create(unique_name);
	if (nullptr == vcm_)
	{
		ump_error("VideoCaptureFactory::Create(unique_name) device_name[%s] unique_name[%s] failed\r\n",
				  device_name, unique_name);
		return -1;
	}

	CaptureSink *capture_file_sink = new CaptureSink();
	for (int i = 0; i < 10; i++)
	{
		vcm_->RegisterCaptureDataCallback(capture_file_sink);
		if (0 != vcm_->StartCapture(capability_))
		{
			ump_error("vcm_ start_capture failed\r\n");
			return -1;
		}

		IntelliUtilSleepMs(1 * 1000);

		ump_info("DeRegisterCaptureDataCallback start\r\n");
		vcm_->DeRegisterCaptureDataCallback();
		ump_info("DeRegisterCaptureDataCallback end\r\n");

		ump_info("StopCapture start\r\n");
		vcm_->StopCapture();
		ump_info("StopCapture end\r\n");
	}

	delete vcm_;
	delete capture_file_sink;
	log_module::uninit_zlog();
	return 0;
}
