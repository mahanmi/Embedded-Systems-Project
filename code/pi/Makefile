# =============================================================================
#  Smart Guardian System -- board daemon
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  Built natively on the Raspberry Pi (four Cortex-A53 cores make `make -j4`
#  faster than setting up a cross toolchain for a project this size).
# =============================================================================

TARGET  := guardian
SRCDIR  := src
OBJDIR  := build

PKGS    := libmicrohttpd libmosquitto json-c sqlite3 libcurl libsystemd

CC      ?= gcc
CFLAGS  := -std=c11 -O2 -g -pipe \
           -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
           -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter \
           -D_GNU_SOURCE \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS := -Wl,-z,relro -Wl,-z,now
LDLIBS  := -lpthread -lm

CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs   $(PKGS))

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean install check

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "built $@ ($$(stat -c%s $@) bytes)"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# Compile-only sanity pass with the analyzer, used before deploying.
check: CFLAGS += -fanalyzer
check: clean all

clean:
	@rm -rf $(OBJDIR) $(TARGET)

PREFIX ?= /opt/guardian
install: $(TARGET)
	install -D -m 0755 $(TARGET)        $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -D -m 0644 www/index.html   $(DESTDIR)$(PREFIX)/www/index.html
	install -D -m 0755 vision/guardian_vision.py $(DESTDIR)$(PREFIX)/bin/guardian_vision.py
	install -D -m 0644 vision/shmframe.py        $(DESTDIR)$(PREFIX)/bin/shmframe.py
	install -D -m 0755 api/guardian_api.py       $(DESTDIR)$(PREFIX)/bin/guardian_api.py

-include $(DEPS)
