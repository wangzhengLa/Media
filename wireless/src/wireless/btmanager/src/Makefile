# btmanager core library — build as libbtmg.so
# Variables expected from parent Makefile:
#   CC, CFLAGS, LDFLAGS, LIBS          — toolchain & flags
#   CONFIG_PREFIX                       — install root (e.g. $(INSTALL_ROOT))
#   DEP_BASE                            — workspace root, sibling packages live here
#   CONFIG_ENABLE_CLASSIC_BLUETOOTH    — y/n
#   CONFIG_ENABLE_LE_BLUETOOTH          — y/n

target = libbtmg.so

# Convert Makefile config vars into C preprocessor macros
ifeq ($(CONFIG_ENABLE_CLASSIC_BLUETOOTH), y)
CPPFLAGS += -DENABLE_CLASSIC_BLUETOOTH
endif
ifeq ($(CONFIG_ENABLE_LE_BLUETOOTH), y)
CPPFLAGS += -DENABLE_LE_BLUETOOTH
endif

# ------------------------------------------------------------------
# Third-party dependency headers (each installed in its own install/root)
# ------------------------------------------------------------------
BLUEZ_DIR     := $(DEP_BASE)/bluez/install/root
BLUEZ_SRC     := $(DEP_BASE)/bluez/src/bluez-5.65
GLIB_DIR      := $(DEP_BASE)/glib/install/root
DBUS_DIR      := $(DEP_BASE)/dbus/install/root
JSONC_DIR     := $(DEP_BASE)/json-c/install/root
ALSA_DIR      := $(DEP_BASE)/alsa-lib/install/root
WMG_SRC_DIR   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../wifimanager/core/include)

INCLUDES += \
	-I . \
	-I ./include \
	-I $(CONFIG_PREFIX)/usr/include \
	-I $(BLUEZ_DIR)/usr/include \
	-I $(BLUEZ_SRC) \
	-I $(GLIB_DIR)/usr/include/glib-2.0 \
	-I $(GLIB_DIR)/usr/lib/glib-2.0/include/ \
	-I $(GLIB_DIR)/usr/include/gio-unix-2.0 \
	-I $(DBUS_DIR)/usr/include/dbus-1.0 \
	-I $(DBUS_DIR)/usr/lib/dbus-1.0/include \
	-I $(JSONC_DIR)/usr/include \
	-I $(ALSA_DIR)/usr/include \
	-I $(WMG_SRC_DIR)

# ------------------------------------------------------------------
# Link libraries — STATIC libs FIRST so their refs can be resolved by
# dynamic libs that follow them (linker only scans .o once).
# ------------------------------------------------------------------
ZLIB_DIR   := $(DEP_BASE)/zlib/install/root
EXPAT_DIR  := $(DEP_BASE)/expat/install/root
LIBFFI_DIR := $(DEP_BASE)/libffi/install/root

# Collect all -L paths for rpath-link (resolve indirect deps like libz, libexpat)
RPATH_LINK  = -Wl,-rpath-link,$(BLUEZ_DIR)/usr/lib
RPATH_LINK += -Wl,-rpath-link,$(GLIB_DIR)/usr/lib
RPATH_LINK += -Wl,-rpath-link,$(DBUS_DIR)/usr/lib
RPATH_LINK += -Wl,-rpath-link,$(JSONC_DIR)/usr/lib
RPATH_LINK += -Wl,-rpath-link,$(ALSA_DIR)/usr/lib
RPATH_LINK += -Wl,-rpath-link,$(ZLIB_DIR)/usr/lib
RPATH_LINK += -Wl,-rpath-link,$(EXPAT_DIR)/usr/lib
RPATH_LINK += -Wl,-rpath-link,$(LIBFFI_DIR)/usr/lib

# 1. Static libs FIRST — they have unresolved refs that dynamic libs below provide
#    Use --whole-archive to pull ALL .o from static libs (btmg.so needs util_malloc,
#    queue_find, bt_uuid*, gatt_db_* etc. that bluez internal .o provide — but if btmg
#    doesn't directly reference them from its own code, the linker would skip util.o etc.)
ifeq ($(CONFIG_ENABLE_LE_BLUETOOTH), y)
LIBS += -Wl,--whole-archive -L$(BLUEZ_DIR)/usr/lib -lshared-mainloop -lbluetooth-internal -Wl,--no-whole-archive
endif

# 2. Dynamic libs AFTER — resolve refs from static libs above
# wirelesscom is called inside btmg by act_* / log_* / snfy_*; must be declared as a NEEDED dependency
LIBS += -Wl,--no-as-needed -L$(CONFIG_PREFIX)/usr/lib -lwirelesscom -Wl,--as-needed
LIBS += -L$(BLUEZ_DIR)/usr/lib -lbluetooth

# json-c is needed by both Classic and LE (bt_config_read_profile uses json_object_from_file)
LIBS += -L$(JSONC_DIR)/usr/lib -ljson-c

# readline/ncurses — bluez internal static libs pull in shell/*.o that reference readline/ncurses
READLINE_DIR := $(DEP_BASE)/readline/install/root
NCURSES_DIR  := $(DEP_BASE)/ncurses/install/root
LIBS += -L$(READLINE_DIR)/usr/lib -L$(NCURSES_DIR)/usr/lib -lreadline -lncursesw -ltinfow

ifeq ($(CONFIG_ENABLE_CLASSIC_BLUETOOTH), y)
LIBS += -L$(GLIB_DIR)/usr/lib -lglib-2.0 -lgio-2.0 -lgobject-2.0
LIBS += -L$(DBUS_DIR)/usr/lib -ldbus-1
LIBS += -L$(ALSA_DIR)/usr/lib -lasound
endif

# ------------------------------------------------------------------
# Source files
# ------------------------------------------------------------------
SRC_ROOT := .

SRC += $(wildcard $(SRC_ROOT)/*.c)
SRC += $(wildcard $(SRC_ROOT)/log/*.c)
SRC += $(wildcard $(SRC_ROOT)/platform/*.c)
SRC += $(wildcard $(SRC_ROOT)/device/bt_adapter.c)

ifeq ($(CONFIG_ENABLE_CLASSIC_BLUETOOTH), y)
SRC += $(wildcard $(SRC_ROOT)/common/*.c)
SRC += $(wildcard $(SRC_ROOT)/device/*.c)
SRC += $(wildcard $(SRC_ROOT)/shared/*.c)
SRC += $(wildcard $(SRC_ROOT)/a2dp/*.c)
SRC += $(wildcard $(SRC_ROOT)/avrcp/*.c)
SRC += $(wildcard $(SRC_ROOT)/spp/*.c)
SRC += $(wildcard $(SRC_ROOT)/hfp/*.c)
endif

ifeq ($(CONFIG_ENABLE_LE_BLUETOOTH), y)
SRC += $(wildcard $(SRC_ROOT)/gatt/*.c)
endif

OBJ = $(patsubst %.c, %.o, $(SRC))

# ------------------------------------------------------------------
# Rules
# ------------------------------------------------------------------
.PHONY: all install clean

all: install

install: $(target)
	-@mkdir -p $(CONFIG_PREFIX)/usr/lib
	-@mkdir -p $(CONFIG_PREFIX)/usr/include
	@cp -f ./include/bt_manager.h  $(CONFIG_PREFIX)/usr/include/bt_manager.h
	@cp -f ./include/bt_log.h      $(CONFIG_PREFIX)/usr/include/bt_log.h
	@cp -f ./include/bt_dev_list.h $(CONFIG_PREFIX)/usr/include/bt_dev_list.h
	@cp -f $(target) $(CONFIG_PREFIX)/usr/lib

# ------------------------------------------------------------------
# Linker flags — clean copy without --as-needed (conflicts with static
# lib groups) and --gc-sections (drops public API not called internally)
# ------------------------------------------------------------------
BTM_LDFLAGS := $(filter-out -Wl,--as-needed -Wl,--gc-sections,$(LDFLAGS))
# Also strip embedded flags inside -Wl,xxx,yyy commas
BTM_LDFLAGS := $(patsubst %--as-needed%,%,$(BTM_LDFLAGS))
BTM_LDFLAGS := $(patsubst %--gc-sections%,%,$(BTM_LDFLAGS))
BTM_LDFLAGS := $(patsubst %,,%,$(BTM_LDFLAGS))   # remove trailing commas

$(target): $(SRC)
	$(CC) -fPIC -g -shared -DOS_NET_LINUX_OS $(CFLAGS) $(INCLUDES) $(CPPFLAGS) $(RPATH_LINK) $(BTM_LDFLAGS) $(LIBS) $^ -o $@

clean:
	rm -f $(target) $(OBJ)
