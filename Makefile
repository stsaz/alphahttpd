# alphahttpd Makefile

ROOT_DIR := ..
AHD_DIR := $(ROOT_DIR)/alphahttpd
FFBASE_DIR := $(ROOT_DIR)/ffbase
FFOS_DIR := $(ROOT_DIR)/ffos
PKG_VER :=
PKG_ARCH :=

include $(FFBASE_DIR)/test/makeconf

BIN := alphahttpd
PKG_DIR := alphahttpd-0
PKG_PACKER := tar -c --owner=0 --group=0 --numeric-owner -v --zstd -f
PKG_EXT := tar.zst
ifeq "$(OS)" "windows"
	BIN := alphahttpd.exe
	PKG_DIR := alphahttpd
	PKG_PACKER := zip -r -v
	PKG_EXT := zip
endif

CFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS += -DFFBASE_HAVE_FFERR_STR
CFLAGS += -I$(AHD_DIR)/src -I$(FFOS_DIR) -I$(FFBASE_DIR)
ifeq "$(DEBUG)" "1"
	CFLAGS += -DFF_DEBUG -O0 -g
# 	CFLAGS += -fsanitize=address
# 	LINKFLAGS := -fsanitize=address
else
	CFLAGS += -O3 -fno-strict-aliasing
	LINKFLAGS := -s
endif
ifneq "$(OLD_CPU)" "1"
	CFLAGS += -march=nehalem
endif
ifeq "$(OS)" "windows"
	LINKFLAGS += -lws2_32
endif

default: build
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) install

build: $(BIN)

DEPS := $(wildcard $(AHD_DIR)/src/*.h) \
	$(wildcard $(AHD_DIR)/src/http/*.h) \
	$(wildcard $(AHD_DIR)/src/util/*.h) \
	$(AHD_DIR)/Makefile

%.o: $(AHD_DIR)/src/%.c $(DEPS)
	$(C) $(CFLAGS) $< -o $@
%.o: $(AHD_DIR)/src/http/%.c $(DEPS)
	$(C) $(CFLAGS) $< -o $@

$(BIN): main.o \
		client.o \
		server.o
	$(LINK) $+ $(LINKFLAGS) $(LINK_PTHREAD) -o $@

clean:
	rm -fv $(BIN) *.o

install:
	mkdir -p $(PKG_DIR)
	cp -ruv $(BIN) $(AHD_DIR)/content-types.conf $(AHD_DIR)/README.md $(AHD_DIR)/www $(PKG_DIR)
ifeq "$(OS)" "windows"
	mv $(PKG_DIR)/README.md $(PKG_DIR)/README.txt
	unix2dos $(PKG_DIR)/README.txt
endif

package: alphahttpd-$(PKG_VER)-$(OS)-$(PKG_ARCH).$(PKG_EXT)

alphahttpd-$(PKG_VER)-$(OS)-$(PKG_ARCH).$(PKG_EXT): $(PKG_DIR)
	$(PKG_PACKER) $@ $<

test: test.o
	$(LINK) $+ $(LINKFLAGS) -o $@
