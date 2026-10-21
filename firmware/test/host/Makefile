# Host tests for the hardware-independent firmware code: make -C firmware/test/host
CC ?= cc
COMP := ../../components
# _GNU_SOURCE as ESP-IDF builds (strlcpy and friends).
CFLAGS := -std=c17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
	-I$(COMP)/xyaoled/include -I$(COMP)/httputil/include -I$(COMP)/imaging/include -I$(COMP)/imaging \
	-I$(COMP)/pngle -I$(COMP)/tjpgd
# Third-party decoders, built without the warnings our code must pass.
# UBSan stays off for them: miniz loads unaligned on x86 by design, TJpgDec's
# IDCT relies on wrapping int arithmetic.
VENDOR_CFLAGS := -std=gnu17 -O1 -g -fsanitize=address -w -DPNGLE_NO_GAMMA_CORRECTION
GO := GOCACHE=$(CURDIR)/$(BUILD)/gocache GOPROXY=off GOTOOLCHAIN=local go
BUILD := build

TESTS := proto upload imaging httputil
IMAGING := $(wildcard $(COMP)/imaging/*.c)

all: $(addprefix $(BUILD)/test_,$(TESTS))
	@for t in $^; do ./$$t || exit 1; done

$(BUILD)/test_proto: test_proto.c test_main.c $(COMP)/xyaoled/xy_proto.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/test_upload: test_upload.c test_main.c $(COMP)/xyaoled/xy_proto.c $(COMP)/xyaoled/xy_upload.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/test_httputil: test_httputil.c test_main.c $(wildcard $(COMP)/httputil/*.c) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/vendor.a: $(COMP)/pngle/pngle.c $(COMP)/pngle/miniz.c $(COMP)/tjpgd/tjpgd.c | $(BUILD)
	cd $(BUILD) && $(CC) $(VENDOR_CFLAGS) -c $(addprefix $(CURDIR)/,$^) && ar rcs vendor.a pngle.o miniz.o tjpgd.o

$(BUILD)/test_imaging: test_imaging.c test_main.c $(IMAGING) $(BUILD)/vendor.a | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

# Decode what the C encoder wrote with Go's image/gif (needs Go; run after the tests).
verify-go: all
	cd gogen && for g in ../$(BUILD)/enc_*.gif; do $(GO) run . verify $$g $${g%.gif}.rgb || exit 1; done

# Regenerate the fixtures from the Go reference logic.
fixtures:
	cd gogen && $(GO) run . gen ../fixtures

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean verify-go fixtures
