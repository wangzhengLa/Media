# btmanager demo/test binary — builds bt_test
# Variables expected from parent Makefile:
#   CC, CFLAGS, LDFLAGS              — toolchain & flags
#   CONFIG_PREFIX                     — install root (e.g. $(INSTALL_ROOT))
#   DEP_BASE                          — workspace root, sibling packages live here
#   CONFIG_ENABLE_CLASSIC_BLUETOOTH   — y/n
#   CONFIG_ENABLE_LE_BLUETOOTH        — y/n

BTM_SRC_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../src)
COMMON_SRC  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../common/src)

# ---- Feature-def macros (must match src/Makefile) ----
ifeq ($(CONFIG_ENABLE_CLASSIC_BLUETOOTH), y)
CPPFLAGS += -DENABLE_CLASSIC_BLUETOOTH
endif
ifeq ($(CONFIG_ENABLE_LE_BLUETOOTH), y)
CPPFLAGS += -DENABLE_LE_BLUETOOTH
endif

# ---- Include paths ----
INCLUDES += \
	-I./include \
	-I$(BTM_SRC_DIR)/include \
	-I$(COMMON_SRC)/api_action \
	-I$(COMMON_SRC)/tracing \
	-I$(COMMON_SRC)/os \
	-I$(COMMON_SRC)/os/linux \
	-I$(CONFIG_PREFIX)/usr/include

BLUEZ_DIR  := $(DEP_BASE)/bluez/install/root
BLUEZ_SRC  := $(DEP_BASE)/bluez/src/bluez-5.65
GLIB_DIR   := $(DEP_BASE)/glib/install/root
DBUS_DIR   := $(DEP_BASE)/dbus/install/root
JSONC_DIR  := $(DEP_BASE)/json-c/install/root
ALSA_DIR   := $(DEP_BASE)/alsa-lib/install/root
READLINE_DIR := $(DEP_BASE)/readline/install/root
NCURSES_DIR  := $(DEP_BASE)/ncurses/install/root

INCLUDES += \
	-I $(BLUEZ_DIR)/usr/include \
	-I $(GLIB_DIR)/usr/include/glib-2.0 \
	-I $(GLIB_DIR)/usr/lib/glib-2.0/include/ \
	-I $(GLIB_DIR)/usr/include/gio-unix-2.0 \
	-I $(DBUS_DIR)/usr/include/dbus-1.0 \
	-I $(DBUS_DIR)/usr/lib/dbus-1.0/include \
	-I $(JSONC_DIR)/usr/include \
	-I $(ALSA_DIR)/usr/include \
	-I $(READLINE_DIR)/usr/include \
	-I $(NCURSES_DIR)/usr/include

# ---- Link paths & libs ----
LIB_PATH := $(CONFIG_PREFIX)/usr/lib

# glib indirect deps (needed at link time when classic BT is enabled)
ZLIB_DIR   := $(DEP_BASE)/zlib/install/root
LIBFFI_DIR := $(DEP_BASE)/libffi/install/root

LIBS += -L$(LIB_PATH) \
	-L$(BLUEZ_DIR)/usr/lib \
	-L$(GLIB_DIR)/usr/lib \
	-L$(DBUS_DIR)/usr/lib \
	-L$(JSONC_DIR)/usr/lib \
	-L$(ALSA_DIR)/usr/lib \
	-L$(READLINE_DIR)/usr/lib \
	-L$(NCURSES_DIR)/usr/lib \
	-lwirelesscom -lbtmg -lbluetooth -ljson-c -ldl -lm -lresolv \
	-lreadline -lncursesw -ltinfow

target = bt_test

C_SRCS := bt_test.c

ifeq ($(CONFIG_ENABLE_CLASSIC_BLUETOOTH), y)
C_SRCS += bt_classic_demo.c
LIBS += -lglib-2.0 -lgio-2.0 -lgobject-2.0 -lasound -ldbus-1
# glib indirect deps (needed at link time)
LIBS += -L$(ZLIB_DIR)/usr/lib -lz
LIBS += -L$(LIBFFI_DIR)/usr/lib -lffi
LIBS += -L$(GLIB_DIR)/usr/lib -lgmodule-2.0
endif

ifeq ($(CONFIG_ENABLE_LE_BLUETOOTH), y)
C_SRCS += bt_gatt_server_demo.c bt_gatt_client_demo.c
LIBS += -Wl,--start-group \
		-L$(BLUEZ_SRC)/src/.libs -L$(BLUEZ_SRC)/lib/.libs \
		-lshared-mainloop -lbluetooth-internal \
		-Wl,--end-group
endif

# Clean LDFLAGS — strip --as-needed (same issue as src/Makefile)
BTM_DEMO_LDFLAGS := $(filter-out -Wl,--as-needed,$(LDFLAGS))
BTM_DEMO_LDFLAGS := $(patsubst %--as-needed%,%,$(BTM_DEMO_LDFLAGS))
BTM_DEMO_LDFLAGS := $(patsubst %,,%,$(BTM_DEMO_LDFLAGS))

C_OBJS := $(C_SRCS:.c=.o)

.PHONY: all install clean

install:
	-@mkdir -p $(CONFIG_PREFIX)/usr/bin

$(C_OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) -g -c -o $@ $^

$(target): $(C_OBJS)
	$(CC) -o $@ $^ $(CPPFLAGS) $(INCLUDES) -g $(LIBS) $(BTM_DEMO_LDFLAGS)
	@cp -f $(target) $(CONFIG_PREFIX)/usr/bin/

all: install $(target)

clean:
	rm -f $(target) $(C_OBJS)
	rm -f *.o
	rm -f bt_test
