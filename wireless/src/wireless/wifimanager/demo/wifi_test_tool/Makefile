INCLUDES += -I $(CONFIG_PREFIX)/usr/include \
			-I ../../core/include \
			-I ../../core/include/linux \

install:
	-@mkdir -p $(CONFIG_PREFIX)/usr/bin

DEAMON_LIBS += -lwifimg-v2.0
DEAMON_LIBS += -lwirelesscom

# BLE deps (pulled transitively via libbtmg.so when CONFIG_WMG_PROTOCOL_BLE=y)
BLUEZ_DIR    := $(DEP_BASE)/bluez/install/root
JSONC_DIR    := $(DEP_BASE)/json-c/install/root
READLINE_DIR := $(DEP_BASE)/readline/install/root
NCURSES_DIR  := $(DEP_BASE)/ncurses/install/root
DEAMON_LIBS += -L$(BLUEZ_DIR)/usr/lib -L$(JSONC_DIR)/usr/lib \
		-L$(READLINE_DIR)/usr/lib -L$(NCURSES_DIR)/usr/lib \
		-lbtmg -lbluetooth -ljson-c -lreadline -lncursesw -ltinfow

DEAMON_SRCS += wifi_daemon.c

# zlog logging library (libwifimg-v2.0.so transitively depends on libzlog.so.1; needed at link time)
ZLOG_DIR := $(DEP_BASE)/zlog/install/root/usr/lib
ZLOG_LINK := -L$(ZLOG_DIR) -Wl,-rpath-link,$(ZLOG_DIR)

ifeq ($(CONFIG_WMG_CMD_SOCKET_ACK), y)
CFLAGS += -DWMG_CMD_SOCKET_ACK
endif

CONFIG_LIST:= \
	CONFIG_WMG_PLATFORM_LINUX \
	CONFIG_WMG_SUPPORT_EXPAND \
	CONFIG_WMG_SUPPORT_STA_MODE \
	CONFIG_WMG_SUPPORT_AP_MODE \
	CONFIG_WMG_SUPPORT_MONITOR_MODE \
	CONFIG_WMG_SUPPORT_P2P_MODE \
	CONFIG_WMG_PROTOCOL_BLE \
	CONFIG_WMG_PROTOCOL_SOFTAP \
	CONFIG_PACKAGE_kmod-net-aic8800 \
	CONFIG_LINUX_4_9 \
	CONFIG_LINUX_5_4 \
	CONFIG_LINUX_5_10 \
	CONFIG_LINUX_5_15 \
	CONFIG_LINUX_6_6 \
	CONFIG_WMG_DEMO_TEST_TOOL \
	CONFIG_WMG_CMD_SOCKET_ACK \
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

wifi_daemon: $(DEAMON_SRCS)
	$(CC) -o $@ $^ $(CFLAGS) $(INCLUDES)  $(LDFLAGS) -L$(CONFIG_PREFIX)/usr/lib -L$(CONFIG_PREFIX)/lib $(ZLOG_LINK) $(DEAMON_LIBS)
	@rm -f $(CONFIG_PREFIX)/usr/bin/wifi_daemon
	@cp -f wifi_daemon $(CONFIG_PREFIX)/usr/bin

wifi: wifi.c
	$(CC) -o $@ $^ $(CFLAGS) $(INCLUDES)  $(LDFLAGS) -L$(CONFIG_PREFIX)/usr/lib $(ZLOG_LINK) -lwifimg-v2.0 -lwirelesscom
	@rm -f $(CONFIG_PREFIX)/usr/bin/wifi
	@cp -f wifi $(CONFIG_PREFIX)/usr/bin
	touch $(CONFVAR)

$(CONFVAR):
	rm -f wifi_daemon wifi .confvar*

all: install $(CONFVAR) wifi_daemon wifi

clean:
	rm -rf wifi_daemon wifi .confvar*
