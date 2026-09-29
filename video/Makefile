include ../platform/config.mk
PROJ_NAME = video
PROJ_VER = 1
PROJ_REL = 2
GIT_BRANCH = master
PKG_NAME = $(PROJ_NAME)-$(PROJ_VER)-$(PROJ_REL)-$(HOST_ARCH)-$(FP_FLAG).pkg.tar.xz
SRC_NAME = $(PROJ_NAME)-$(PROJ_VER).tar.xz
PROJ_NAME_VER = $(PROJ_NAME)-$(PROJ_VER)
PWD := $(shell pwd)
default: build

fetchSrc:
	@if [ -d src ]; then \
		echo "Error: src directory exists."; \
		exit 1; \
	fi
	git clone -b $(GIT_BRANCH) $(GIT_SRC_URL):$(GIT_MAINTAINER)/$(PROJ_NAME).git src/$(PROJ_NAME)

configure:
	cd src/$(PROJ_NAME) && \
	rm -rf build && \
	mkdir build && \
	cd build && \
	cmake ../ \
	-DUSE_AM311X_PLATFORM=ON \
	-DUSE_LIVE555_RTSP=OFF \
	-DENABLE_ISP=OFF \
	-DENABLE_WEBSOCKETS=ON \
	-DCMAKE_INSTALL_PREFIX=$(PLATFORM_DIR)/../$(PROJ_NAME)/install/root \
	-DCMAKE_SYSTEM_NAME=Linux \
	-DCMAKE_C_COMPILER=$(CROSS_COMPILE)gcc  \
	-DCMAKE_CXX_COMPILER=$(CROSS_COMPILE)g++ 
         
build:
	$(MAKE) -C src/$(PROJ_NAME)/build -j 4

clean:
	$(MAKE) -C src/$(PROJ_NAME)/build clean
	
distclean:
	$(MAKE) -C src/$(PROJ_NAME)/build clean

install:
	-$(MAKE) -C src/$(PROJ_NAME)/build install
	rm -rf install/root/usr/lib/pkgconfig
	
strip: install
	@echo "=== Starting strip process ==="
	
	# 1. 处理所有动态库（如果存在）
	if [ -d install/root/usr/lib ]; then \
		echo "Stripping shared libraries..."; \
		find install/root/usr/lib -name "*.so*" -type f -exec $(CROSS_COMPILE)strip {} \; -print; \
	else \
		echo "No library directory found (statically linked?)"; \
	fi
	
	# 2. 处理所有 bin 目录下的可执行文件
	if [ -d install/root/usr/bin ]; then \
		echo "Stripping executables in bin directory..."; \
		for exe in install/root/usr/bin/*; do \
			if [ -f "$$exe" ] && [ -x "$$exe" ]; then \
				orig_size=$$(stat -c%s "$$exe" 2>/dev/null); \
				echo "Stripping: $$exe"; \
				$(CROSS_COMPILE)strip "$$exe"; \
				new_size=$$(stat -c%s "$$exe" 2>/dev/null); \
				echo "  $$(numfmt --to=iec $$orig_size) -> $$(numfmt --to=iec $$new_size)"; \
				ls -lh "$$exe"; \
			fi \
		done \
	else \
		echo "No bin directory found"; \
	fi
	
	# 3. 显示最终结果（添加错误处理）
	@echo "=== Installation complete ==="
	@echo "Binary directory contents:"
	@if [ -d install/root/usr/bin ]; then \
		ls -la install/root/usr/bin/; \
	else \
		echo "  (bin directory not found)"; \
	fi
	
	@echo ""
	@echo "Library directory contents:"
	@if [ -d install/root/usr/lib ]; then \
		ls -la install/root/usr/lib/; \
	else \
		echo "  (no libraries installed - static linking)"; \
	fi
	
	@echo ""
	@echo "Total installation size: $$(du -sh install/root | cut -f1)"
	
PKG:
	-rm $(PKG_NAME)
	cd install/root && tar Jcf ../../$(PKG_NAME) *

fetchPkg:
	$(call fetchPkgTemplate,$(PKG_NAME),install/root)

put-pkg:
	curl -u "${JFROG_USERNAME}:${JFROG_PASSWORD}" -T "${PKG_NAME}" "${JFROG_ARTIFACTORY_BASE_TAR}/${BOARD}/" --connect-timeout 10 --max-time 300

push-src:
	curl -u "${JFROG_USERNAME}:${JFROG_PASSWORD}" -T "${SRC_NAME}" "${JFROG_ARTIFACTORY_OPEN_SOURCE}/${BOARD}/" --connect-timeout 10 --max-time 300

crossInstall:
	mkdir -p $(CROSS_INSTALL_USR)/
	mkdir -p $(CROSS_INSTALL_ROOT)/etc/conf
	cp -dR install/root/usr/* $(CROSS_INSTALL_USR)/
	cp -dR install/root/etc/conf/* $(CROSS_INSTALL_ROOT)/etc/conf

crossInstallSecure:
	mkdir -p $(CROSS_INSTALL_RAMDISK)/
	cp -dRf install/root/* $(CROSS_INSTALL_RAMDISK)/
		
disFetchSrc:
	rm -rf src

disFetchPkg:
	rm -rf install $(PKG_NAME)

disFetchAll:disFetchSrc disFetchPkg

.PHONY:install clean distclean

