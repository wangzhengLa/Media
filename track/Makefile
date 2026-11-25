include ../platform/config.mk

PROJ_NAME  = track
GIT_BRANCH = main
GIT_VER    := $(shell cat VERSION)
PROJ_VER   = $(GIT_BRANCH)
PROJ_REL   = 2
PKG_NAME   = $(PROJ_NAME)-$(PROJ_VER)-$(GIT_VER)-$(PROJ_REL)-$(HOST_ARCH)-$(FP_FLAG).pkg.tar.xz
PROJECT    = ${PROJECT_TYPE}

PWD        := $(shell pwd)
SRC_DIR    := src/$(PROJ_NAME)
INSTALL_ROOT := $(PWD)/install/root

CXX        := $(CROSS_COMPILE)g++
STRIP      := $(CROSS_COMPILE)strip

CXXFLAGS   := $(COMMON_CFLAGS) -std=c++17 -Wall -Wextra -fno-omit-frame-pointer -I$(SRC_DIR)/include
LDFLAGS    := $(COMMON_LDFLAGS) -lpthread

TARGET     := build/$(PROJ_NAME)
SRCS       := $(wildcard $(SRC_DIR)/src/*.cpp)
CONF_DIR   := $(SRC_DIR)/conf

default: build

fetchSrc:
	@if [ -d src ]; then \
		echo "Error: src directory exists."; \
		exit 1; \
	fi
	git clone -b $(GIT_BRANCH) $(GIT_SRC_URL):$(GIT_MAINTAINER)/$(PROJ_NAME).git src/$(PROJ_NAME)

configure:
	@echo "No configuration needed."

build: $(TARGET)

$(TARGET): $(SRCS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

update_git_ver:
	NEW_HASH=$$(git -C src/$(PROJ_NAME) rev-parse --short HEAD); \
	echo "$$NEW_HASH" > VERSION;

install: update_git_ver build
	rm -rf install
	mkdir -p $(INSTALL_ROOT)/usr/bin
	mkdir -p $(INSTALL_ROOT)/etc/$(PROJ_NAME)
	mkdir -p $(INSTALL_ROOT)/etc/init.d
	cp $(TARGET) $(INSTALL_ROOT)/usr/bin/
	$(STRIP) $(INSTALL_ROOT)/usr/bin/$(PROJ_NAME)
	cp $(CONF_DIR)/$(PROJ_NAME).conf $(INSTALL_ROOT)/etc/$(PROJ_NAME)/
	cp $(CONF_DIR)/S55$(PROJ_NAME) $(INSTALL_ROOT)/etc/init.d/
	chmod +x $(INSTALL_ROOT)/etc/init.d/S55$(PROJ_NAME)

clean:
	rm -rf build

distclean: clean
	rm -rf install $(PKG_NAME)

PKG:
	-rm -f $(PKG_NAME)
	cd install/root && tar Jcf ../../$(PKG_NAME) *

fetchPkg:
	$(call fetchPkgTemplate,$(PKG_NAME),install/root)

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
	mkdir -p $(CROSS_INSTALL_ROOT)/usr/bin
	mkdir -p $(CROSS_INSTALL_ROOT)/etc/$(PROJ_NAME)
	mkdir -p $(CROSS_INSTALL_ROOT)/etc/init.d
	cp -p install/root/usr/bin/$(PROJ_NAME) $(CROSS_INSTALL_ROOT)/usr/bin/
	cp -p install/root/etc/$(PROJ_NAME)/$(PROJ_NAME).conf $(CROSS_INSTALL_ROOT)/etc/$(PROJ_NAME)/
	cp -p install/root/etc/init.d/S55$(PROJ_NAME) $(CROSS_INSTALL_ROOT)/etc/init.d/
	chmod +x $(CROSS_INSTALL_ROOT)/etc/init.d/S55$(PROJ_NAME)

disFetchSrc:
	rm -rf src

disFetchPkg:
	rm -rf install $(PKG_NAME)

disFetchAll: disFetchSrc disFetchPkg

.PHONY: default fetchSrc configure build update_git_ver install clean distclean PKG fetchPkg put-pkg crossInstall disFetchSrc disFetchPkg disFetchAll
