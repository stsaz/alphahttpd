# alphahttpd Makefile

AHD_DIR := .
FFBASE_DIR := ../ffbase
FFOS_DIR := ../ffos

include $(FFBASE_DIR)/test/makeconf

CFLAGS := -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS += -I$(AHD_DIR)/src -I$(FFOS_DIR) -I$(FFBASE_DIR)
ifeq "$(OPT)" "0"
	CFLAGS += -DFF_DEBUG -O0 -g
else
	CFLAGS += -O3 -fno-strict-aliasing
endif
ifneq "$(SSE42)" "0"
	CFLAGS += -msse4.2
endif
LDFLAGS := -s

all: alphahttpd

DEPS := $(wildcard $(AHD_DIR)/src/*.h) \
	$(wildcard $(AHD_DIR)/src/http/*.h) \
	$(wildcard $(AHD_DIR)/src/util/*.h) \
	$(AHD_DIR)/Makefile

%.o: $(AHD_DIR)/src/%.c $(DEPS)
	$(C) $(CFLAGS) $< -o $@
%.o: $(AHD_DIR)/src/http/%.c $(DEPS)
	$(C) $(CFLAGS) $< -o $@

alphahttpd: \
		client.o \
		main.o \
		server.o
	$(LINK) $(LDFLAGS) $+ -o $@

clean:
	rm -fv alphahttpd *.o
