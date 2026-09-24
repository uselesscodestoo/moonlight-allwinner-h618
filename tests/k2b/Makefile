CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
CPPFLAGS ?=

.PHONY: test-fb-unblank
test-fb-unblank: build/k2b-tests/test_fb_unblank
	./build/k2b-tests/test_fb_unblank

build/k2b-tests/test_fb_unblank: tests/k2b/test_fb_unblank.c tools/k2b-fb-unblank.c
	mkdir -p build/k2b-tests
	$(CC) $(CFLAGS) $< -o $@

BUILD_DIR := build/k2b-tests
EXEEXT :=
ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif
TEST_BINARY := $(BUILD_DIR)/test_frame$(EXEEXT)
AU_TEST_BINARY := $(BUILD_DIR)/test_access_unit$(EXEEXT)
QUEUE_TEST_BINARY := $(BUILD_DIR)/test_input_queue$(EXEEXT)
DISP_TEST_BINARY := $(BUILD_DIR)/test_disp_config$(EXEEXT)
MEMORY_TEST_BINARY := $(BUILD_DIR)/test_cedar_memory$(EXEEXT)
COMPAT_TEST_BINARY := $(BUILD_DIR)/test_cedar54_compat$(EXEEXT)
COMPAT_WRAPS := $(foreach call,getenv uname stat fstat open read close syscall,-Wl,--wrap=$(call))
CEDARC_HEADER := $(K2B_CEDARC_HEADERS)/sc_interface.h
CEDAR_HEADER := $(K2B_VENDOR_CEDAR_HEADERS)/cedar_ve.h
MEMORY_HEADER_DEPS = $(subst $(space),\$(space),$(wildcard $(subst $(space),\$(space),$(CEDARC_HEADER)))) $(subst $(space),\$(space),$(wildcard $(subst $(space),\$(space),$(CEDAR_HEADER))))
MEMORY_WRAPS := $(foreach call,open read close fstat uname sysconf ioctl mmap munmap,-Wl,--wrap=$(call))

# Keep the frame-only target independent of the external vendor include tree.
empty :=
space := $(empty) $(empty)
VENDOR_HEADER := $(K2B_VENDOR_HEADERS)/video/sunxi_display2.h
VENDOR_HEADER_DEP := $(subst $(space),\$(space),$(wildcard $(subst $(space),\$(space),$(VENDOR_HEADER))))

.PHONY: test
test: $(TEST_BINARY)
	./$(TEST_BINARY)

$(TEST_BINARY): tests/k2b/test_frame.c src/video/k2b/frame.c src/video/k2b/frame.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/video/k2b tests/k2b/test_frame.c src/video/k2b/frame.c $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: test-au
test-au: $(AU_TEST_BINARY)
	./$(AU_TEST_BINARY)

$(AU_TEST_BINARY): tests/k2b/test_access_unit.c src/video/k2b/access_unit.c src/video/k2b/access_unit.h third_party/moonlight-common-c/src/Limelight.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/video/k2b -Ithird_party/moonlight-common-c/src tests/k2b/test_access_unit.c src/video/k2b/access_unit.c $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: test-queue
test-queue: $(QUEUE_TEST_BINARY)
	./$(QUEUE_TEST_BINARY)

$(QUEUE_TEST_BINARY): tests/k2b/test_input_queue.c src/video/k2b/input_queue.c src/video/k2b/input_queue.h src/video/k2b/access_unit.c src/video/k2b/access_unit.h third_party/moonlight-common-c/src/Limelight.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -Isrc/video/k2b -Ithird_party/moonlight-common-c/src tests/k2b/test_input_queue.c src/video/k2b/input_queue.c src/video/k2b/access_unit.c $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: test-disp check-vendor-headers
test-disp: $(DISP_TEST_BINARY)
	./$(DISP_TEST_BINARY)

RETIRE_TEST_BINARY := $(BUILD_DIR)/test_disp_retire$(EXEEXT)
.PHONY: test-disp-retire
test-disp-retire: $(RETIRE_TEST_BINARY)
	./$(RETIRE_TEST_BINARY)
	./$(RETIRE_TEST_BINARY) timeout

$(RETIRE_TEST_BINARY): tests/k2b/test_disp_retire.c src/video/k2b/disp_presenter.c src/video/k2b/disp_presenter.h $(VENDOR_HEADER_DEP) | check-vendor-headers
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/video/k2b -idirafter "$(K2B_VENDOR_HEADERS)" tests/k2b/test_disp_retire.c $(LDFLAGS) $(LDLIBS) -o $@

# An order-only phony prerequisite validates the path even for a cached binary.
check-vendor-headers:
	@test -n "$(K2B_VENDOR_HEADERS)" || { echo "ERROR: test-disp requires K2B_VENDOR_HEADERS=<vendor kernel include directory>" >&2; exit 1; }
	@test -f "$(VENDOR_HEADER)" || { echo "ERROR: vendor header not found: $(VENDOR_HEADER)" >&2; exit 1; }

$(DISP_TEST_BINARY): tests/k2b/test_disp_config.c src/video/k2b/frame.c src/video/k2b/frame.h src/video/k2b/disp_config.c src/video/k2b/disp_config.h src/video/k2b/disp_uapi.h $(VENDOR_HEADER_DEP) | check-vendor-headers
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/video/k2b -idirafter "$(K2B_VENDOR_HEADERS)" tests/k2b/test_disp_config.c src/video/k2b/frame.c src/video/k2b/disp_config.c $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: test-memory check-memory-headers
test-memory: $(MEMORY_TEST_BINARY)
	./$(MEMORY_TEST_BINARY)

check-memory-headers:
	@test -n "$(K2B_CEDARC_HEADERS)" -a -f "$(CEDARC_HEADER)" || { echo "ERROR: test-memory requires K2B_CEDARC_HEADERS containing sc_interface.h" >&2; exit 1; }
	@test -n "$(K2B_VENDOR_CEDAR_HEADERS)" -a -f "$(CEDAR_HEADER)" || { echo "ERROR: test-memory requires K2B_VENDOR_CEDAR_HEADERS containing cedar_ve.h" >&2; exit 1; }
	@$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -Isrc/video/k2b -I"$(K2B_CEDARC_HEADERS)" -I"$(K2B_VENDOR_CEDAR_HEADERS)" -fsyntax-only src/video/k2b/cedar_memory.c

$(MEMORY_TEST_BINARY): tests/k2b/test_cedar_memory.c src/video/k2b/cedar_memory.c src/video/k2b/cedar_memory.h src/video/k2b/cedar_uapi.h $(MEMORY_HEADER_DEPS) | check-memory-headers
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -Isrc/video/k2b -I"$(K2B_CEDARC_HEADERS)" -I"$(K2B_VENDOR_CEDAR_HEADERS)" tests/k2b/test_cedar_memory.c src/video/k2b/cedar_memory.c $(MEMORY_WRAPS) $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: test-compat
test-compat: $(COMPAT_TEST_BINARY)
	./$(COMPAT_TEST_BINARY)

$(COMPAT_TEST_BINARY): tests/k2b/test_cedar54_compat.c src/video/k2b/cedar54_compat.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/video/k2b tests/k2b/test_cedar54_compat.c src/video/k2b/cedar54_compat.c $(COMPAT_WRAPS) $(LDFLAGS) $(LDLIBS) -o $@

RUNTIME_TEST_BINARY := $(BUILD_DIR)/test_cedar_runtime$(EXEEXT)
RUNTIME_WRAPS := $(foreach call,getenv realpath stat lstat dlopen dlsym dlerror dladdr dlinfo dl_iterate_phdr dlclose,-Wl,--wrap=$(call))
RUNTIME_HEADERS := include/vdecoder.h include/typedef.h include/sc_interface.h include/veInterface.h include/vbasetype.h include/cdc_config.h vdecoder/include/videoengine.h vdecoder/include/sbm.h vdecoder/include/sbmInterface.h vdecoder/include/fbm.h base/include/cdc_log.h base/include/CdcMessageQueue.h
RUNTIME_INCLUDES = -I"$(K2B_CEDARC_ROOT)/include" -I"$(K2B_CEDARC_ROOT)/base/include" -I"$(K2B_CEDARC_ROOT)/vdecoder/include"
RUNTIME_HEADER_DEPS = $(foreach header,$(RUNTIME_HEADERS),$(wildcard $(K2B_CEDARC_ROOT)/$(header)))

.PHONY: test-runtime check-runtime-headers
test-runtime: $(RUNTIME_TEST_BINARY)
	./$(RUNTIME_TEST_BINARY)

# Validate the external headers even when the executable is already cached.
check-runtime-headers:
	@test -n "$(K2B_CEDARC_ROOT)" || { echo "ERROR: test-runtime requires K2B_CEDARC_ROOT=<fixed CedarC source directory>" >&2; exit 1; }
	@for header in $(RUNTIME_HEADERS); do test -f "$(K2B_CEDARC_ROOT)/$$header" || { echo "ERROR: required CedarC header missing: $$header" >&2; exit 1; }; done

$(RUNTIME_TEST_BINARY): tests/k2b/test_cedar_runtime.c src/video/k2b/cedar_runtime.c src/video/k2b/cedar_runtime.h src/video/k2b/cedar_memory.h $(RUNTIME_HEADER_DEPS) | check-runtime-headers
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTINA_LINUX_SUPPORT=0 -pthread -Isrc/video/k2b $(RUNTIME_INCLUDES) tests/k2b/test_cedar_runtime.c src/video/k2b/cedar_runtime.c $(RUNTIME_WRAPS) $(LDFLAGS) $(LDLIBS) -ldl -o $@

PICTURE_TEST_BINARY := $(BUILD_DIR)/test_cedar_picture$(EXEEXT)
VIDEO_PREDECODE_TEST_BINARY := $(BUILD_DIR)/test_video_predecode$(EXEEXT)

.PHONY: test-video-predecode
test-video-predecode: $(VIDEO_PREDECODE_TEST_BINARY)
	./$(VIDEO_PREDECODE_TEST_BINARY)
	./$(VIDEO_PREDECODE_TEST_BINARY) watchdog

.PHONY: test-audio-shutdown
test-audio-shutdown: $(BUILD_DIR)/test_audio_shutdown
	./$(BUILD_DIR)/test_audio_shutdown normal
	./$(BUILD_DIR)/test_audio_shutdown wait
	./$(BUILD_DIR)/test_audio_shutdown stop
	./$(BUILD_DIR)/test_audio_shutdown cleanup

$(BUILD_DIR)/test_audio_shutdown: tests/k2b/test_audio_shutdown.c src/audio/alsa.c
	mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-pointer-sign -DHAVE_K2B -Ithird_party/moonlight-common-c/src $$(pkg-config --cflags alsa opus) $< $$(pkg-config --libs alsa opus) -o $@

$(VIDEO_PREDECODE_TEST_BINARY): tests/k2b/test_video_predecode.c src/video/k2b/video.c src/video/k2b/input_queue.c src/video/k2b/access_unit.c src/video/k2b/cedar_picture.c src/video/k2b/frame.c $(RUNTIME_HEADER_DEPS) | check-runtime-headers
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTINA_LINUX_SUPPORT=0 -pthread -Isrc/video/k2b -Ithird_party/moonlight-common-c/src $(RUNTIME_INCLUDES) tests/k2b/test_video_predecode.c src/video/k2b/input_queue.c src/video/k2b/access_unit.c src/video/k2b/cedar_picture.c src/video/k2b/frame.c $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: test-picture
test-picture: $(PICTURE_TEST_BINARY)
	./$(PICTURE_TEST_BINARY)

$(PICTURE_TEST_BINARY): tests/k2b/test_cedar_picture.c src/video/k2b/cedar_picture.c src/video/k2b/cedar_picture.h src/video/k2b/cedar_memory.h src/video/k2b/frame.c src/video/k2b/frame.h $(RUNTIME_HEADER_DEPS) | check-runtime-headers
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTINA_LINUX_SUPPORT=0 -Isrc/video/k2b $(RUNTIME_INCLUDES) tests/k2b/test_cedar_picture.c src/video/k2b/cedar_picture.c src/video/k2b/frame.c $(LDFLAGS) $(LDLIBS) -o $@
