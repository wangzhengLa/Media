#include "camera.h"

MIPI_SENSOR_INFO_S SENSOR_1LANE_F37_30FPS_10BIT_LINEAR_INFO =
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
			1080,
			0x40,
			0,
			/*entry_index, mode, reg_width, sensor_name, extra_mode*/
			1,
			1,
			8,
			"f37",
			0,
			/*deserial_index deserial_port*/
			-1,
			0,
			{0, 0, 0} /* spi_info */
		}};

MIPI_SENSOR_INFO_S SENSOR_1LANE_F37_30FPS_10BIT_DOL2_INFO =
{
	.deseEnable = 0,
	.inputMode = INPUT_MODE_MIPI,
	.sensorInfo =
		{
			/* port,dev_port,bus_type, bus_num, fps, resolution, sensor_addr, serial_addr*/
			0,
			0,
			0,
			1,
			30,
			1080,
			0x40,
			0,
			/*entry_index, mode, reg_width, sensor_name, extra_mode*/
			0,
			DOL2_M,
			8,
			"f37",
			0,
			/*deserial_index deserial_port*/
			-1,
			0,
			{0, 0, 0} /* spi_info */
		}};

MIPI_ATTR_S MIPI_1LANE_SENSOR_F37_30FPS_10BIT_NORMAL_ATTR =
	{
		.mipi_host_cfg =
			{
				1,	  /* lane */
				0x2b, /* datatype */
				24,	  /* mclk	 */
				864,  /* mipiclk */
				30,	  /* fps */
				1920, /* width	*/
				1080, /* height */
				2560, /* linlength */
				1125, /* framelength */
				20	  /* settle */
			},
		.dev_enable = 0 /*  mipi dev enable */
};

MIPI_ATTR_S MIPI_1LANE_SENSOR_F37_30FPS_10BIT_DOL2_ATTR =
{
	.mipi_host_cfg = {
	    .lane = 1,
	    .datatype = 0x2b,
	    .mclk = 24,
	    .mipiclk = 864,
	    .fps = 30,
	    .width = 1920,
	    .height = 1080,
	    .linelenth = 2560,
	    .framelenth = 1125,
	    .settle = 20,
		.channel_num= 2,
		.channel_sel = {0, 1}
	},
	.dev_enable = 0
};

VIN_DEV_ATTR_S DEV_ATTR_F37_LINEAR_BASE = {
	.stSize = {
		0,	  /*format*/
		1920, /*width*/
		1080, /*height*/
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
			.width = 1920,
			.height = 1080,
			.pix_length = 1,
		}},
	.outDdrAttr = {
		.stride = 2400,
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
	}};

VIN_DEV_ATTR_S DEV_ATTR_F37_DOL2_BASE = {
    .stSize = {
		.format = 0,
		.width = 1920,
		.height = 1080,
		.pix_length = 1
    },
    .mipiAttr = {
        .enable = 1,
        .ipi_channels = 2,
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
            .width = 1920,
            .height = 1080,
            .pix_length = 1,
        }
    },
    .outDdrAttr = {
        .stride = 2400,
        .buffer_num = 8,
    },
    .outIspAttr = {
        .dol_exp_num = 2,
        .enable_dgain = 0,
        .set_dgain_short = 0,
        .set_dgain_medium = 0,
        .set_dgain_long = 0,
        .vc_short_seq = 0,
        .vc_medium_seq = 0,
        .vc_long_seq = 1,
    }
};

VIN_DEV_ATTR_S DEV_ATTR_F37_LINEAR_FEEDBACK = {
	.stSize = {
		0,	  /*format*/
		1920, /*width*/
		1080, /*height*/
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
		.raw_feedback_en = 1,
		.data = {
			.format = 0,
			.width = 1920,
			.height = 1080,
			.pix_length = 1,
		}},
	.outDdrAttr = {
		.stride = 2400,
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
	}};

VIN_DEV_ATTR_EX_S DEV_ATTR_F37_MD_BASE = {
	.path_sel = 0,
	.roi_top = 0,
	.roi_left = 0,
	.roi_width = 1280,
	.roi_height = 640,
	.grid_step = 128,
	.grid_tolerance = 10,
	.threshold = 10,
	.weight_decay = 128,
	.precision = 0};

VIN_PIPE_ATTR_S PIPE_ATTR_F37_LINEAR_BASE = {
	.ddrOutBufNum = 6,
	.snsMode = SENSOR_NORMAL_MODE,
	.stSize = {
		.format = 0,
		.width = 1920,
		.height = 1080,
	},
	.snsMode = 1,
	.cfaPattern = 3,
	.temperMode = 2,
	.ispBypassEn = 0,
	.ispAlgoState = 1,
	.bitwidth = 10,
	.calib = {
		.mode = 1,
		.lname = "/etc/cam/libjxf37_liner.so",
	}};

	VIN_PIPE_ATTR_S PIPE_ATTR_F37_DOL2_BASE = {
	.ddrOutBufNum = 6,
	.snsMode = SENSOR_DOL2_MODE,
	.stSize = {
		.format = 0,
		.width = 1920,
		.height = 1080,
	},
	.snsMode = 2,
	.cfaPattern = 3,
	.temperMode = 2,
	.ispBypassEn = 0,
	.ispAlgoState = 1,
	.bitwidth = 12,
	.calib = {
		.mode = 1,
		.lname = "/etc/cam/libjxf37_hdr.so",
	}
};