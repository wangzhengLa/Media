#include "camera.h"

MIPI_SENSOR_INFO_S SENSOR_1LANE_SC1330T_30FPS_10BIT_LINEAR_INFO =
{
	.deseEnable = 0,
	.inputMode = INPUT_MODE_MIPI,
	.sensorInfo =
		{
			.port = 0,  // sensor \u903b\u8f91\u7f16\u53f7\uff0c\u5fc5\u987b\u4ece 0 \u5f00\u59cb
            .dev_port = 0,  // sensor \u9a71\u52a8\u8282\u70b9, \u4e00\u4e2a\u9a71\u52a8\u652f\u6301\u591a\u4e2a\u8282\u70b9 dev_port \u5fc5\u987b \u7b49\u4e8e pipe_id
            .bus_type = 0,  // 0 -> i2c, 1 -> spi
            .bus_num = 5,  // \u603b\u7ebf\u53f7 i2c5
            .fps = 30,  //  \u5e27\u7387
            .resolution = 960,  // sensor \u884c\u6570\uff0c\u5fc5\u987b\u4e0e mipi \u5c5e\u6027\u4e00\u81f4
            .sensor_addr = 0x30,  // i2c \u8bbe\u5907\u5730\u5740
            .serial_addr = 0,
            .entry_index = 1,  // mipi host \u53f7
            .sensor_mode = NORMAL_M,  // \u5de5\u4f5c\u6a21\u5f0f\uff0c1 -> normal, 2 -> dol2, 3 -> dol3
            .reg_width = 16,  // \u5bc4\u5b58\u5668\u5730\u5740\u5bbd\u5ea6
            .sensor_name = "sc1330t",  // \u4e0e\u9a71\u52a8\u5e93\u540d\u5bf9\u5e94
            .extra_mode = 0,
            .deserial_index = -1,
            .deserial_port = 0,
		}};

MIPI_SENSOR_INFO_S SENSOR_1LANE_SC1330T_30FPS_10BIT_DOL2_INFO =
{
	.deseEnable = 0,
	.inputMode = INPUT_MODE_MIPI,
	.sensorInfo =
	{
		.port = 0,  // sensor \u903b\u8f91\u7f16\u53f7\uff0c\u5fc5\u987b\u4ece 0 \u5f00\u59cb
            .dev_port = 0,  // sensor \u9a71\u52a8\u8282\u70b9, \u4e00\u4e2a\u9a71\u52a8\u652f\u6301\u591a\u4e2a\u8282\u70b9 dev_port \u5fc5\u987b \u7b49\u4e8e pipe_id
            .bus_type = 0,  // 0 -> i2c, 1 -> spi
            .bus_num = 5,  // \u603b\u7ebf\u53f7 i2c5
            .fps = 30,  //  \u5e27\u7387
            .resolution = 960,  // sensor \u884c\u6570\uff0c\u5fc5\u987b\u4e0e mipi \u5c5e\u6027\u4e00\u81f4
            .sensor_addr = 0x30,  // i2c \u8bbe\u5907\u5730\u5740
            .serial_addr = 0,
            .entry_index = 1,  // mipi host \u53f7
            .sensor_mode = DOL2_M,  // \u5de5\u4f5c\u6a21\u5f0f\uff0c1 -> normal, 2 -> dol2, 3 -> dol3
            .reg_width = 16,  // \u5bc4\u5b58\u5668\u5730\u5740\u5bbd\u5ea6
            .sensor_name = "sc1330t",  // \u4e0e\u9a71\u52a8\u5e93\u540d\u5bf9\u5e94
            .extra_mode = 0,
            .deserial_index = -1,
            .deserial_port = 0,
	}
};

MIPI_ATTR_S MIPI_1LANE_SENSOR_SC1330T_30FPS_10BIT_NORMAL_ATTR =
	{
		.mipi_host_cfg =
			{
				.lane = 1,
	            .datatype = 0x2b,  // 0x29->raw8 0x2B->raw10 0x2c->raw12
	            .mclk = 24,  // mipi \u4e3b\u65f6\u949f
	            .mipiclk = 504,  // \u603b mipibit rate Mbits/s
	            .fps = 30,  // sensor \u5b9e\u9645\u8f93\u51fa\u5e27\u7387
	            .width = 1280,
	            .height = 960,
	            .linelenth = 1600,  // sensor \u8f93\u51fa\u5e26 blanking \u7684\u603b\u884c\u957f
	            .framelenth = 1050,  // sensor \u8f93\u51fa\u5e26 blanking \u7684\u603b\u884c\u6570
	            .settle = 20,  // sensor \u5b9e\u9645 Ttx-zero + Ttx-prepare \u65f6\u95f4 (clk \u4e3a\u5355\u4f4d)
			},
		.dev_enable = 0 /*  mipi dev enable */
};

MIPI_ATTR_S MIPI_1LANE_SENSOR_SC1330T_30FPS_10BIT_DOL2_ATTR =
{
	.mipi_host_cfg = {
	    .lane = 1,
	    .datatype = 0x2b,
	    .mclk = 24,
	    .mipiclk = 990,
	    .fps = 30,
	    .width = 1280,
	    .height = 960,
	    .linelenth = 1650,
	    .framelenth = 2000,
	    .settle = 20,
		.channel_num= 2,
		.channel_sel = {0, 1}
	},
	.dev_enable = 0
};

VIN_DEV_ATTR_S DEV_ATTR_SC1330T_LINEAR_BASE = {
	.stSize = {
		.format = 0,  // 0->raw
            .width = 1280,
            .height = 960,
            .pix_length = 1,  // 0->raw8, 1->raw10, 2->raw12
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
	}};

VIN_DEV_ATTR_S DEV_ATTR_SC1330T_DOL2_BASE = {
    .stSize = {
		.format = 0,
		.width = 1280,
		.height = 960,
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
            .width = 1280,
            .height = 960,
            .pix_length = 1,
        }
    },
    .outDdrAttr = {
        .stride = 1600,
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

VIN_DEV_ATTR_S DEV_ATTR_SC1330T_LINEAR_FEEDBACK = {
	.stSize = {
		.format = 0,  // 0->raw
            .width = 1920,
            .height = 1080,
            .pix_length = 1,  // 0->raw8, 1->raw10, 2->raw12
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

VIN_DEV_ATTR_EX_S DEV_ATTR_SC1330T_MD_BASE = {
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

VIN_PIPE_ATTR_S PIPE_ATTR_SC1330T_LINEAR_BASE = {
	.ddrOutBufNum = 6,
	.snsMode = SENSOR_NORMAL_MODE,
	.stSize = {
		.format = 0,
		.width = 1280,
		.height = 960,
	},
	.cfaPattern = 3,
	.temperMode = 2,
	.ispBypassEn = 0,
	.ispAlgoState = 1,
	.bitwidth = 10,
	.calib = {
		.mode = 1,
#ifdef ECO_X2
		.lname = "libsc1330t_linear.so",
#else
		.lname = "/etc/cam/libsc1330t_linear.so",
#endif
	}
};

VIN_PIPE_ATTR_S PIPE_ATTR_SC1330T_DOL2_BASE = {
	.ddrOutBufNum = 6,
	.snsMode = SENSOR_DOL2_MODE,
	.stSize = {
		.format = 0,
		.width = 1280,
		.height = 960,
	},
	.cfaPattern = 3,
	.temperMode = 2,
	.ispBypassEn = 0,
	.ispAlgoState = 1,
	.bitwidth = 10,
	.calib = {
		.mode = 1,
#ifdef ECO_X2
		.lname = "libsc1330t_hdr.so.V1",
#else
		.lname = "/etc/cam/libsc1330t_hdr.so",
#endif
	}
};