override CC := $(if $(filter default,$(origin CC)),aarch64-linux-gnu-gcc,$(CC))
BUILD_DIR ?= build
PACKAGE_DIR ?= $(BUILD_DIR)/package
PORT_DIR := $(PACKAGE_DIR)/ports/theswapper
LIB_DIR := $(BUILD_DIR)/libs.aarch64
TOOL_DIR := $(BUILD_DIR)/tools.aarch64
ZIP := $(BUILD_DIR)/theswapper.zip

CFLAGS ?= -O2
CFLAGS += -std=c11 -D_GNU_SOURCE -fPIC -Wall -Wextra -Werror -Wno-strict-prototypes
LDFLAGS ?=
LDFLAGS += -s

NATIVE_LIBS := \
  $(LIB_DIR)/libwindows_stub.so \
  $(LIB_DIR)/libsteam_api.so \
  $(LIB_DIR)/libsdkencryptedappticket.so \
  $(LIB_DIR)/libfmodex.so

NATIVE_TOOLS := \
  $(TOOL_DIR)/normalmap-downscale

all: $(NATIVE_LIBS) $(NATIVE_TOOLS) aliases

$(LIB_DIR):
	mkdir -p $@

$(TOOL_DIR):
	mkdir -p $@

$(LIB_DIR)/libwindows_stub.so: src/windows_stub.c | $(LIB_DIR)
	$(CC) $(CFLAGS) -shared $(LDFLAGS) -o $@ $<

$(LIB_DIR)/libsteam_api.so: src/steam_stub.c | $(LIB_DIR)
	$(CC) $(CFLAGS) -shared $(LDFLAGS) -o $@ $<

$(LIB_DIR)/libsdkencryptedappticket.so: src/steam_stub.c | $(LIB_DIR)
	$(CC) $(CFLAGS) -shared $(LDFLAGS) -o $@ $<

$(LIB_DIR)/libfmodex.so: src/fmodex_sdl_mixer.c | $(LIB_DIR)
	$(CC) $(CFLAGS) -shared $(LDFLAGS) -o $@ $< -ldl

$(TOOL_DIR)/normalmap-downscale: src/normalmap_downscale.c | $(TOOL_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -ldl -lm

aliases: $(LIB_DIR)/libwindows_stub.so
	cp -f $< $(LIB_DIR)/kernel32
	cp -f $< $(LIB_DIR)/dwmapi
	cp -f $< $(LIB_DIR)/libkernel32.so
	cp -f $< $(LIB_DIR)/libdwmapi.so

package: all
	rm -rf "$(PACKAGE_DIR)"
	mkdir -p "$(PORT_DIR)"
	cp -R packaging/ports/theswapper/. "$(PORT_DIR)/"
	cp -R "$(LIB_DIR)/." "$(PORT_DIR)/theswapper/libs.aarch64/"
	cp -R "$(TOOL_DIR)/." "$(PORT_DIR)/theswapper/tools/"
	chmod +x "$(PORT_DIR)/The Swapper.sh" "$(PORT_DIR)/theswapper/tools/setup.sh" \
	  "$(PORT_DIR)/theswapper/tools/xdg-open" "$(PORT_DIR)/theswapper/tools/normalmap-downscale"

zip: package
	rm -f "$(ZIP)"
	cd "$(PACKAGE_DIR)" && zip -qr "../theswapper.zip" ports

docker-build:
	docker build -t theswapper-port-build .
	docker run --rm -v "$$PWD":/src theswapper-port-build make clean all

clean:
	rm -rf "$(BUILD_DIR)"

.PHONY: all aliases package zip docker-build clean
