CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
CPPFLAGS ?=

BUILD_DIR := build/k2b-tests
EXEEXT :=
ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif
TEST_BINARY := $(BUILD_DIR)/test_frame$(EXEEXT)
DISP_TEST_BINARY := $(BUILD_DIR)/test_disp_config$(EXEEXT)

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
