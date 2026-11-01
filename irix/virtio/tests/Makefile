# Host tests of the driver's kernel-independent code.
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
CPPFLAGS += -I../../driver
OUT ?= /tmp/virtio-host-tests

test: $(OUT)/test_virtq
	$(OUT)/test_virtq

$(OUT)/test_virtq: test_virtq.c ../../driver/virtq.c ../../driver/virtq.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_virtq.c ../../driver/virtq.c

clean:
	rm -rf $(OUT)

.PHONY: test clean
