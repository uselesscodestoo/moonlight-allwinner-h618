CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
CPPFLAGS ?=

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
