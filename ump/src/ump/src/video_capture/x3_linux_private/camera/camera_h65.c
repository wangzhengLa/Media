#include "camera.h"

MIPI_SENSOR_INFO_S SENSOR_1LANE_H65_30FPS_10BIT_LINEAR_INFO =
{
	.deseEnable = 0,
	.inputMode = INPUT_MODE_MIPI,
	.sensorInfo =
		{
			/* port,dev_port,bus_type, bus_num, fps, resolution, sensor_addr, serial_addr*/
			0,
			0,
			0,
			5,
			30,
			960,
			0x30,
			0,
			/*entry_index, mode, reg_width, sensor_name, extra_mode*/
			1,
			1,
			8,
			"h65",
			0,
			/*deserial_index deserial_port*/
			-1,
			0,
			{0, 0, 0} /* spi_info */
		}
};


MIPI_ATTR_S MIPI_1LANE_SENSOR_H65_30FPS_10BIT_NORMAL_ATTR =
	{
		.mipi_host_cfg =
			{
				1,	  /* lane */
				0x2b, /* datatype */
				24,	  /* mclk	 */
				432,  /* mipiclk */
				30,	  /* fps */
				1280, /* width	*/
				960, /* height */
				1440, /* linlength */
				1000, /* framelength */
				20	  /* settle */
			},
		.dev_enable = 0 /*  mipi dev enable */
};

VIN_DEV_ATTR_S DEV_ATTR_H65_LINEAR_BASE = {
	.stSize = {
		0,	  /*format*/
		1280, /*width*/
		960, /*height*/
		1	  /*pix_length*/
	},
	.mipiAttr = {
		.enable = 1,
		.ipi_channels = 1,
		.enable_mux_out = 1,
		.enable_frame_id = 1,
		.enable_bypass = 0,
		.enable_line_shift = 0,
		.enable_id_decoder = 0,
		.set_init_frame_id = 1,
		.set_line_shift_count = 0,
		.set_bypass_channels = 1,
	},
	.DdrIspAttr = {
		.buf_num = 4,
		.raw_feedback_en = 0,
		.data = {
			.format = 0,
			.width = 1280,
			.height = 960,
			.pix_length = 1,
		}},
	.outDdrAttr = {
		.stride = 1600,
		.buffer_num = 8,
	},
	.outIspAttr = {
		.dol_exp_num = 1,
		.enable_dgain = 0,
		.set_dgain_short = 0,
		.set_dgain_medium = 0,
		.set_dgain_long = 0,
		.vc_short_seq = 0,
		.vc_medium_seq = 0,
		.vc_long_seq = 0,
	}
};

// VIN_PIPE_ATTR_S PIPE_ATTR_H65_LINEAR_BASE = {
// 	.ddrOutBufNum = 6,
// 	.snsMode = SENSOR_NORMAL_MODE,
// 	.stSize = {
// 		.format = 0,
// 		.width = 1280,
// 		.height = 960,
// 	},
// 	.snsMode = 1,
// 	.cfaPattern = 3,
// 	.temperMode = 2,
// 	.ispBypassEn = 0,
// 	.ispAlgoState = 1,
// 	.bitwidth = 10,
// 	.calib = {
// 		.mode = 1,
// 		.lname = "/etc/cam/libjxh65_liner.so",
// 	}
// };

VIN_PIPE_ATTR_S PIPE_ATTR_H65_LINEAR_BASE = {
	.ddrOutBufNum = 6,
	.snsMode = SENSOR_NORMAL_MODE,
	.stSize = {
		.format = 0,
		.width = 1280,
		.height = 960,
	},
	.snsMode = 1,
	.cfaPattern = 0,
	.temperMode = 2,
	.ispBypassEn = 0,
	.ispAlgoState = 1,
	.bitwidth = 10,
	.calib = {
		.mode = 1,
		.lname = "/etc/cam/libjxh65_linear.so",
	}
};

