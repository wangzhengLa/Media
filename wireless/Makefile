include ../platform/config.mk

PROJ_NAME   = wireless
GIT_BRANCH  = master
GIT_VER     := $(shell cat VERSION)
PROJ_VER    = $(GIT_BRANCH)
PROJ_REL    = 7
PKG_NAME    = $(PROJ_NAME)-$(PROJ_VER)-$(GIT_VER)-$(PROJ_REL)-$(HOST_ARCH)-$(FP_FLAG).pkg.tar.xz
PWD         := $(shell pwd)

SRC_DIR       = src/$(PROJ_NAME)
COMMON_DIR    = $(SRC_DIR)/common/src
WMG_CORE_DIR  = $(SRC_DIR)/wifimanager/core
WMG_DEMO_DIR  = $(SRC_DIR)/wifimanager/demo/wifi_test_tool
WMG_FILES_DIR = $(SRC_DIR)/wifimanager/files
BTM_SRC_DIR   = $(SRC_DIR)/btmanager/src
BTM_DEMO_DIR  = $(SRC_DIR)/btmanager/demo
BTM_CONFIG_DIR = $(SRC_DIR)/btmanager/config
INSTALL_ROOT  = $(PWD)/install/root

# BT feature switches
BTM_ENABLE_CLASSIC ?= n
BTM_ENABLE_LE      ?= y

WMG_MODE_CFLAGS := -DSUPPORT_STA_MODE -DSUPPORT_AP_MODE -DSUPPORT_LINKD

# DEP_BASE — workspace root
DEP_BASE = $(PWD)/..

WMG_PLATFORM_PARAMS = \
	CONFIG_PREFIX=$(INSTALL_ROOT) \
	CONFIG_ARCH=$(CPU_ARCH) \
	CONFIG_LIBC=glibc \
	CONFIG_WMG_PLATFORM_LINUX=y \
	CONFIG_WMG_SUPPORT_STA_MODE=y \
	CONFIG_WMG_SUPPORT_AP_MODE=y \
	CONFIG_WMG_SUPPORT_MONITOR_MODE=n \
	CONFIG_WMG_SUPPORT_P2P_MODE=n \
	CONFIG_WMG_SUPPORT_EXPAND=y \
	CONFIG_WMG_DEMO_TEST_TOOL=y \
	CONFIG_WMG_CMD_SOCKET_ACK=y \
	CONFIG_LINUX_5_10=y \
	CONFIG_WMG_PROTOCOL_SOFTAP=y \
	CONFIG_WMG_PROTOCOL_BLE=$(BTM_ENABLE_LE) \
	CONFIG_ENABLE_LE_BLUETOOTH=$(BTM_ENABLE_LE) \
	DEP_BASE=$(DEP_BASE)

BTM_PLATFORM_PARAMS = \
	CONFIG_PREFIX=$(INSTALL_ROOT) \
	DEP_BASE=$(DEP_BASE) \
	CONFIG_ENABLE_CLASSIC_BLUETOOTH=$(BTM_ENABLE_CLASSIC) \
	CONFIG_ENABLE_LE_BLUETOOTH=$(BTM_ENABLE_LE)

WMG_CFLAGS_EXTRA = -Os -fPIC -ffunction-sections -fdata-sections \
	-DOS_NET_LINUX_OS -DWMG_CONFIG_PATH='"/etc/wifi"' \
	-DWIRELESS_MODULE_AIC8800 $(WMG_MODE_CFLAGS)
WMG_LDFLAGS_EXTRA = -Wl,--as-needed -Wl,--gc-sections

# 从 COMMON_CONFIG_ENV 提取工具链变量（CC/CXX/AR/RANLIB/NM/STRIP/CXXFLAGS，不含 CFLAGS/LDFLAGS）
WMG_TOOLCHAIN_ENV = \
	CC=$(MR536_ROOTFS_CROSS_COMPILE)gcc \
	CXX=$(MR536_ROOTFS_CROSS_COMPILE)g++ \
	AR=$(MR536_ROOTFS_CROSS_COMPILE)ar \
	RANLIB=$(MR536_ROOTFS_CROSS_COMPILE)ranlib \
	NM=$(MR536_ROOTFS_CROSS_COMPILE)nm \
	STRIP=$(MR536_ROOTFS_CROSS_COMPILE)strip \
	CXXFLAGS="$(TARGET_CXXFLAGS)"

# CFLAGS/LDFLAGS 用 export（带 C 宏字符串字面量引号，命令行传递会被 shell 吃掉）
export CFLAGS  := $(TARGET_CFLAGS) $(WMG_CFLAGS_EXTRA)
export LDFLAGS := $(TARGET_LDFLAGS) $(WMG_LDFLAGS_EXTRA)
export LIBS    := -lpthread -lrt

default: build

DEPS = \
    bluez:fetchPkg \
    glib:fetchPkg \
    dbus:fetchPkg \
    json-c:fetchPkg \
    readline:fetchPkg \
    ncurses:fetchPkg \
    zlib:fetchPkg \
    libffi:fetchPkg \
    expat:fetchPkg \
    zlog:fetchPkg

fetchDeps:
	@echo ">>> Fetching all dependencies (fetchPkg)..."
	@fail=0; \
	for dep in $(DEPS); do \
		dir=$$(echo $$dep | cut -d: -f1); \
		target=$$(echo $$dep | cut -d: -f2); \
		if [ ! -d ../$$dir ]; then \
			echo "Warning: ../$$dir does not exist, skipping"; \
			continue; \
		fi; \
		echo ">>> [$$dir] make $$target"; \
		$(MAKE) -C ../$$dir $$target || { echo "Error: $$dir $$target failed"; fail=1; }; \
	done; \
	if [ $$fail -ne 0 ]; then echo "Some dependencies failed to fetch"; exit 1; fi
	@echo ">>> All dependencies fetched successfully"

# bluez 需要额外拉源码（wireless 用到 src/bluez-5.65/ 下的头文件和 .libs/ 静态库）
fetchBluezSrc:
	@echo ">>> Fetching bluez source (fetchSrc)..."
	$(MAKE) -C ../bluez fetchSrc || echo "Warning: bluez source may already exist"

# 一键拉齐所有依赖 + bluez 源码
fetchAll: fetchDeps fetchBluezSrc

fetchSrc: fetchAll
	@if [ -d src ]; then \
		echo "Error: src directory exists."; \
		exit 1; \
	fi
	git clone -b $(GIT_BRANCH) $(GIT_SRC_URL):$(GIT_MAINTAINER)/$(PROJ_NAME).git src/$(PROJ_NAME)

configure:
	@echo "No configuration needed for wifi_manager."

build: 
	@rm -f $(COMMON_DIR)/libwirelesscom.so   $(COMMON_DIR)/.confvar*
	@rm -f $(WMG_CORE_DIR)/libwifimg-v2.0.so $(WMG_CORE_DIR)/.confvar*
	@rm -f $(WMG_DEMO_DIR)/wifi_daemon $(WMG_DEMO_DIR)/wifi $(WMG_DEMO_DIR)/.confvar*
	@rm -f $(BTM_SRC_DIR)/libbtmg.so
	@rm -f $(BTM_DEMO_DIR)/bt_test
	$(MAKE) -C $(COMMON_DIR)   $(WMG_TOOLCHAIN_ENV) $(WMG_PLATFORM_PARAMS) all
	$(MAKE) -C $(BTM_SRC_DIR)   $(WMG_TOOLCHAIN_ENV) $(BTM_PLATFORM_PARAMS) all
	$(MAKE) -C $(WMG_CORE_DIR) $(WMG_TOOLCHAIN_ENV) $(WMG_PLATFORM_PARAMS) all
	$(MAKE) -C $(BTM_DEMO_DIR)  $(WMG_TOOLCHAIN_ENV) $(BTM_PLATFORM_PARAMS) all
	$(MAKE) -C $(WMG_DEMO_DIR) $(WMG_TOOLCHAIN_ENV) $(WMG_PLATFORM_PARAMS) all

install: update_git_ver
	@mkdir -p $(INSTALL_ROOT)/usr/lib
	@mkdir -p $(INSTALL_ROOT)/usr/include
	@mkdir -p $(INSTALL_ROOT)/usr/bin
	@mkdir -p $(INSTALL_ROOT)/etc/wifi
	@mkdir -p $(INSTALL_ROOT)/etc/wifi/hostapd/sockets
	@mkdir -p $(INSTALL_ROOT)/etc/wifi/wpa_supplicant/sockets
	@mkdir -p $(INSTALL_ROOT)/etc/bluetooth
	@cp -f $(WMG_FILES_DIR)/wifimg.config           $(INSTALL_ROOT)/etc/wifi/
	@cp -f $(WMG_FILES_DIR)/hostapd.conf            $(INSTALL_ROOT)/etc/wifi/hostapd/
	@cp -f $(WMG_FILES_DIR)/udhcpd.conf             $(INSTALL_ROOT)/etc/wifi/     # softap mode uses udhcpd (busybox DHCP server)
	@cp -f $(WMG_FILES_DIR)/wpa_supplicant.conf        $(INSTALL_ROOT)/etc/wifi/wpa_supplicant/
	@cp -f $(WMG_FILES_DIR)/wmg_udhcpc.script      $(INSTALL_ROOT)/etc/wifi/
	@cp -f $(WMG_FILES_DIR)/wmg_udhcpc6.script     $(INSTALL_ROOT)/etc/wifi/
	@cp -f $(WMG_FILES_DIR)/update_conf.sh         $(INSTALL_ROOT)/etc/wifi/
	@mkdir -p $(INSTALL_ROOT)/etc/conf
	@cp -f $(WMG_FILES_DIR)/wmg_log.conf           $(INSTALL_ROOT)/etc/conf/
	@chmod +x $(INSTALL_ROOT)/etc/wifi/*.sh $(INSTALL_ROOT)/etc/wifi/*.script
	@cp -f $(BTM_CONFIG_DIR)/bluetooth.json $(INSTALL_ROOT)/etc/bluetooth/
	@cp -f $(BTM_CONFIG_DIR)/bt_init.sh     $(INSTALL_ROOT)/etc/bluetooth/
	@chmod +x $(INSTALL_ROOT)/etc/bluetooth/bt_init.sh

update_git_ver:
	NEW_HASH=$$(git -C src/$(PROJ_NAME) rev-parse --short HEAD); \
	echo "$$NEW_HASH" > VERSION;

clean:
	-$(MAKE) -C $(COMMON_DIR)   clean
	-$(MAKE) -C $(BTM_SRC_DIR)   clean
	-$(MAKE) -C $(BTM_DEMO_DIR)  clean
	-$(MAKE) -C $(WMG_CORE_DIR) clean
	-$(MAKE) -C $(WMG_DEMO_DIR) clean

distclean: clean
	rm -rf install

strip: install
	@echo "=== Stripping shared libraries ==="
	@if [ -d $(INSTALL_ROOT)/usr/lib ]; then \
		find $(INSTALL_ROOT)/usr/lib -name "*.so*" -type f -exec $(CROSS_COMPILE)strip {} \; -print; \
	fi
	@echo "=== Stripping executables ==="
	@if [ -d $(INSTALL_ROOT)/usr/bin ]; then \
		for exe in $(INSTALL_ROOT)/usr/bin/*; do \
			if [ -f "$$exe" ] && [ -x "$$exe" ]; then \
				$(CROSS_COMPILE)strip "$$exe"; \
				echo "  stripped: $$exe"; \
			fi; \
		done; \
	fi

PKG:
	-rm $(PKG_NAME)
	cd $(INSTALL_ROOT) && tar Jcf ../../$(PKG_NAME) *

fetchPkg:
	$(call fetchPkgTemplate,$(PKG_NAME),$(INSTALL_ROOT))

put-pkg:
	@HTTP_CODE=$$(curl -s -o /dev/null -w "%{http_code}" -u "${JFROG_USERNAME}:${JFROG_PASSWORD}" -I "${JFROG_ARTIFACTORY_BASE_TAR}/${BOARD}/${PKG_NAME}"); \
	if [ "$$HTTP_CODE" -eq 200 ]; then \
		echo "error: [${PKG_NAME}] existence in jfrog, cannot upload overwriting!!!"; \
		exit 1; \
	else \
		curl -u "${JFROG_USERNAME}:${JFROG_PASSWORD}" -T "${PKG_NAME}" "${JFROG_ARTIFACTORY_BASE_TAR}/${BOARD}/" --connect-timeout 10 --max-time 300; \
		if [ $$? -ne 0 ]; then \
			echo "upload fail，please check"; \
			exit 1; \
		fi; \
	fi

crossInstall:
	@mkdir -p $(CROSS_INSTALL_USR)/
	@cp -dRf $(INSTALL_ROOT)/usr/* $(CROSS_INSTALL_USR)/
	@mkdir -p $(CROSS_INSTALL_ROOT)/etc/wifi
	@cp -dRf $(INSTALL_ROOT)/etc/wifi/* $(CROSS_INSTALL_ROOT)/etc/wifi/
	@mkdir -p $(CROSS_INSTALL_ROOT)/etc/bluetooth
	@cp -dRf $(INSTALL_ROOT)/etc/bluetooth/* $(CROSS_INSTALL_ROOT)/etc/bluetooth/
	@mkdir -p $(CROSS_INSTALL_ROOT)/etc/conf
	@cp -dR install/root/etc/conf/* $(CROSS_INSTALL_ROOT)/etc/conf

crossInstallSecure:
	@mkdir -p $(CROSS_INSTALL_RAMDISK)/
	@cp -dRf $(INSTALL_ROOT)/* $(CROSS_INSTALL_RAMDISK)/

disFetchSrc:
	rm -rf src
disFetchPkg:
	rm -rf install $(PKG_NAME)
disFetchAll: disFetchSrc disFetchPkg

.PHONY: default configure build install clean distclean strip PKG fetchPkg put-pkg crossInstall crossInstallSecure fetchSrc fetchDeps fetchBluezSrc fetchAll disFetchSrc disFetchPkg disFetchAll update_git_ver
