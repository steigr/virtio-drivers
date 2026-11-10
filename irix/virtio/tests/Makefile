# Host test of the split rings, the transport layer's kernel-independent code.
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
CPPFLAGS += -I..
OUT ?= /tmp/virtio-host-tests

test: $(OUT)/test_virtq
	$(OUT)/test_virtq

$(OUT)/test_virtq: test_virtq.c ../virtq.c ../virtq.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_virtq.c ../virtq.c

clean:
	rm -f $(OUT)/test_virtq

.PHONY: test clean
