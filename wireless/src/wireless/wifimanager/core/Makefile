target = libwifimg-v2.0.so

INCLUDES        += -I ./include
INCLUDES        += -I ./include/os
INCLUDES        += -I ./include/os/linux
INCLUDES        += -I src/os/linux/linkd
INCLUDES        += -I $(CONFIG_PREFIX)/usr/include
# zlog logging library (prebuilt libzlog.so)
INCLUDES        += -I $(DEP_BASE)/zlog/install/root/usr/include
LIBS            += -Wl,--no-as-needed -L$(DEP_BASE)/zlog/install/root/usr/lib -lzlog -Wl,--as-needed

SRCS += src/wifimg.c
SRCS += src/wmg_common.c
SRCS += src/log/wifi_log.c
SRCS += src/config/wmg_get_config.c
ifeq ($(CONFIG_WMG_SUPPORT_EXPAND), y)
SRCS += src/expand_cmd.c
SRCS += src/os/linux/expand/linux_expand_cmd.c
endif

ifeq ($(CONFIG_WMG_SUPPORT_STA_MODE), y)
SRCS += src/wmg_sta.c
SRCS += src/os/linux/linux_get_config.c
SRCS += src/os/linux/udhcpc.c
SRCS += src/os/linux/event.c
SRCS += src/os/linux/linux_sta.c
SRCS += src/os/linux/linux_common.c
SRCS += src/os/linux/scan.c
SRCS += src/os/linux/utils.c
SRCS += src/os/linux/linux_support_list.c
endif

ifneq ($(CONFIG_WMG_PROTOCOL_BLE)$(CONFIG_WMG_PROTOCOL_SOFTAP), y)
SRCS += src/linkd.c
endif


ifeq ($(CONFIG_WMG_SUPPORT_AP_MODE), y)
SRCS += src/wmg_ap.c
SRCS += src/os/linux/linux_ap.c
endif

ifeq ($(CONFIG_WMG_SUPPORT_MONITOR_MODE), y)
SRCS += src/wmg_monitor.c
SRCS += src/os/linux/linux_monitor.c
endif

ifeq ($(CONFIG_WMG_SUPPORT_P2P_MODE), y)
SRCS += src/wmg_p2p.c
SRCS += src/os/linux/linux_p2p.c
endif

############### Linux platform protocol ###############
ifeq ($(CONFIG_WMG_PROTOCOL_SOFTAP), y)
SRCS += src/os/linux/linkd/linkd_softap/linkd_softap.c
endif

ifeq ($(CONFIG_WMG_PROTOCOL_BLE), y)
BTM_SRC_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../btmanager/src)
BLUEZ_DIR := $(DEP_BASE)/bluez/install/root
BLUEZ_SRC := $(DEP_BASE)/bluez/src/bluez-5.65
JSONC_DIR := $(DEP_BASE)/json-c/install/root

INCLUDES += \
	-I $(BTM_SRC_DIR)/include \
	-I $(BLUEZ_DIR)/usr/include \
	-I $(BLUEZ_SRC) \
	-I $(JSONC_DIR)/usr/include

SRCS += src/os/linux/linkd/linkd_ble/linkd_ble.c
# wirelesscom is called inside wifimg by act_* / os_net_*; must be declared as a NEEDED dependency
LIBS += -Wl,--no-as-needed -L$(CONFIG_PREFIX)/usr/lib -lwirelesscom -Wl,--as-needed
LIBS += -Wl,--no-as-needed -L$(CONFIG_PREFIX)/usr/lib -lbtmg -Wl,--as-needed \
	-L$(BLUEZ_DIR)/usr/lib -lbluetooth \
	-L$(JSONC_DIR)/usr/lib -ljson-c \
	-ldl -lm -lresolv

RPATH_LINK = -Wl,-rpath-link,$(BLUEZ_DIR)/usr/lib \
             -Wl,-rpath-link,$(JSONC_DIR)/usr/lib
endif
############### Linux platform protocol ###############


LOCAL_CFLAGS = -D_GNU_SOURCE=1

ifeq ($(CONFIG_LINUX_4_9), y)
LOCAL_CFLAGS += -DLINUX_VERSION_4_9
endif
ifeq ($(CONFIG_LINUX_5_4), y)
LOCAL_CFLAGS += -DLINUX_VERSION_5_4
endif
ifeq ($(CONFIG_LINUX_5_10), y)
LOCAL_CFLAGS += -DLINUX_VERSION_5_10
endif
ifeq ($(CONFIG_LINUX_5_15), y)
LOCAL_CFLAGS += -DLINUX_VERSION_5_15
endif
ifeq ($(CONFIG_LINUX_6_6), y)
LOCAL_CFLAGS += -DLINUX_VERSION_6_6
endif

install:
	-@mkdir -p $(CONFIG_PREFIX)/usr/lib
	-@mkdir -p $(CONFIG_PREFIX)/usr/include
	@cp ./include/*.h $(CONFIG_PREFIX)/usr/include

# static libwpa_client is selected precisely by ARCH + LIBC to avoid findstring substring mis-matching
# (findstring arm, aarch64 would match; must check aarch64 before arm)
ARCH_aarch64 := $(filter aarch64%, $(CONFIG_ARCH))
ARCH_arm     := $(filter arm%, $(CONFIG_ARCH))
ARCH_riscv32 := $(filter riscv32%, $(CONFIG_ARCH))
ARCH_riscv64 := $(filter riscv64%, $(CONFIG_ARCH))

#use glibc libc
ifeq ($(findstring glibc, $(CONFIG_LIBC)), glibc)
    ifneq ($(ARCH_aarch64),)
        SRCS += ./src/os/linux/lib/glibc/libwpa_client_glibc_aarch64.a
    else ifneq ($(ARCH_arm),)
        ifeq ($(CONFIG_SOFT_FLOAT), y)
            SRCS += ./src/os/linux/lib/glibc/libwpa_client_glibc_arm_softft.a
        else
            SRCS += ./src/os/linux/lib/glibc/libwpa_client_glibc_arm.a
        endif
    else ifneq ($(ARCH_riscv32),)
        SRCS += ./src/os/linux/lib/glibc/libwpa_client_glibc_riscv32.a
    else ifneq ($(ARCH_riscv64),)
        SRCS += ./src/os/linux/lib/glibc/libwpa_client_glibc_riscv64.a
    endif
else #use musl libc
    ifneq ($(ARCH_aarch64),)
        #no added yet, need the corresponding tool chain to compile
        SRCS += ./src/os/linux/lib/musl/libwpa_client_musl_aarch64.a
    else ifneq ($(ARCH_arm),)
        ifeq ($(CONFIG_SOFT_FLOAT), y)
            #no added yet, need the corresponding tool chain to compile
            SRCS += ./src/os/linux/lib/musl/libwpa_client_musl_arm_softft.a
        else
            SRCS += ./src/os/linux/lib/musl/libwpa_client_musl_arm.a
        endif
    else ifneq ($(ARCH_riscv32),)
        SRCS += ./src/os/linux/lib/musl/libwpa_client_musl_riscv32.a
    else ifneq ($(ARCH_riscv64),)
        SRCS += ./src/os/linux/lib/musl/libwpa_client_musl_riscv64.a
    endif
endif

CONFIG_LIST:= \
	CONFIG_WMG_SUPPORT_EXPAND \
	CONFIG_WMG_SUPPORT_STA_MODE \
	CONFIG_WMG_SUPPORT_AP_MODE \
	CONFIG_WMG_SUPPORT_MONITOR_MODE \
	CONFIG_WMG_SUPPORT_P2P_MODE \
	CONFIG_WMG_PROTOCOL_BLE \
	CONFIG_WMG_PROTOCOL_SOFTAP \
	CONFIG_LINUX_4_9 \
	CONFIG_LINUX_5_4 \
	CONFIG_LINUX_5_10 \
	CONFIG_LINUX_5_15 \
	CONFIG_LINUX_6_6 \
	CONFIG_WMG_DEMO_TEST_TOOL \
	CONFIG_WMG_DEMO_STA \
	CONFIG_WMG_DEMO_AP \
	CONFIG_WMG_DEMO_MONITOR \
	CONFIG_WMG_DEMO_P2P \
	CONFIG_WMG_DEMO_STA_AP_COEXIST \
	CONFIG_WMG_DEMO_NET_CONFIG \
	CONFIG_WMG_DEMO_FRAME

empty:=
space:= $(empty) $(empty)
CONFVAR_=$(foreach v,$(CONFIG_LIST),$(if $(findstring y,$($(v))),y,_))
CONFVAR=.confvar-$(subst $(space),,$(CONFVAR_))


$(target): $(SRCS) $(ADTLIB)
	$(CC) -fPIC -shared $(INCLUDES) $(CPPFLAGS) $(LOCAL_CFLAGS) $(LDFLAGS) $(RPATH_LINK) $(CFLAGS) $^ -o $@ $(LIBS)
	@cp $@ $(CONFIG_PREFIX)/usr/lib
	touch $(CONFVAR)

$(CONFVAR):
	rm -f $(target) .confvar*

all: install $(CONFVAR) $(target)

clean:
	rm -rf $(target) .confvar*
