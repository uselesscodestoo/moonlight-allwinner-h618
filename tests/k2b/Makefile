CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
CPPFLAGS ?=

BUILD_DIR := build/k2b-tests
EXEEXT :=
ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif
TEST_BINARY := $(BUILD_DIR)/test_frame$(EXEEXT)

.PHONY: test
test: $(TEST_BINARY)
	./$(TEST_BINARY)

$(TEST_BINARY): tests/k2b/test_frame.c src/video/k2b/frame.c src/video/k2b/frame.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/video/k2b tests/k2b/test_frame.c src/video/k2b/frame.c $(LDFLAGS) $(LDLIBS) -o $@
