# hms -- Hypervisor Management System.
#
# Source the QNX SDP environment first (qnxsdp-env.sh), then:
#
#     make
#
# libmosquitto is not in this repository, so say where it is:
#
#     make MQTT_INCDIR=/opt/mosquitto/include MQTT_LIBDIR=/opt/mosquitto/lib
#
# The defaults assume it is installed under /usr/local.
#
# This used to source qnxsdp-env.sh itself, from ../../qnx800, and call qcc
# through the $QNX_HOST that produced. That worked only inside the monorepo this
# was split out of, and it took the choice of compiler away from anything
# driving the build. The environment is the caller's job now.

# qcc with the target variant, which is what the SDP environment provides.
#
# NOT `CC ?= ...`. make predefines CC as "cc", so its origin is 'default' rather
# than 'undefined' and ?= leaves it alone -- the build then silently uses the
# host gcc and fails on the first QNX header, which reads like a missing SDP.
ifeq ($(origin CC),default)
CC := qcc -Vgcc_ntoaarch64le
endif

MQTT_INCDIR ?= /usr/local/include
MQTT_LIBDIR ?= /usr/local/lib

# CFLAGS is overridable so a build system can supply its own optimisation and
# warning settings. The flags this code requires are kept out of it, so
# overriding CFLAGS cannot drop them.
CFLAGS     ?= -O2 -Wall -Wextra
APP_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_ALL_SOURCE

# Threads.
#
# QNX carries pthreads in libc, so the target build needs no flag at all -- and
# qcc is not gcc, so handing it -pthread is a way to break the build for no
# benefit. A host toolchain does need it (it is what defines _REENTRANT and
# links libpthread), and building on the host is how these sources get checked
# without an SDP to hand:
#
#     make CC=gcc THREAD_FLAGS=-pthread MQTT_INCDIR=... MQTT_LIBDIR=...
#
# Default empty, which is what the target wants.
THREAD_FLAGS ?=

CPPFLAGS += -I$(MQTT_INCDIR)
LDFLAGS  += -L$(MQTT_LIBDIR)

# -lcjson used to be on this line, with an -I to match, and nothing here calls
# cJSON. ssh and scp are run as commands rather than linked, so there is no
# libssh either -- libmosquitto is the only real dependency.
LDLIBS := -lmosquitto -lsocket -lm

BUILD_DIR := build
TARGET    := $(BUILD_DIR)/hms

# cli/menu.c is deliberately absent: it is not part of the program this Makefile
# has ever built, and adding it would be a decision rather than an omission
# being corrected.
SRCS := main.c \
        guest/guest.c guest/discoverer.c guest/lifecycle.c guest/address.c \
        ssh/client.c ssh/session_pool.c ssh/shell.c \
        config/config.c \
        utils/proc.c \
        mqtt/mqtt_client.c \
        ota/ota.c

OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean deploy

all: $(TARGET)

# Per-object rules rather than one command over every source, so a rebuild
# after touching one file compiles one file.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(APP_CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(THREAD_FLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

# Copy hms and its configuration to a running host.
# Usage: make deploy HOST=root@192.168.2.2
#
# The libmosquitto copy the old version of this target did is gone: under Yocto
# the library is part of the image, and for a hand build it belongs wherever
# MQTT_LIBDIR points rather than being pushed to /lib behind the image's back.
HOST ?= root@192.168.2.2

deploy: all
	scp $(TARGET) $(HOST):/bin/hms
	scp config/hms.conf $(HOST):/etc/hms.conf
	@echo "Deployed. Run 'hms' on the host (verify with: ldd /bin/hms)"

clean:
	rm -rf $(BUILD_DIR)
