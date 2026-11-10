# Host test of the 9P2000.L client, the filesystem's kernel-independent code.
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
CPPFLAGS += -I.. -I../../virtio
OUT ?= /tmp/virtio-host-tests

test: $(OUT)/test_p9
	$(OUT)/test_p9

$(OUT)/test_p9: test_p9.c ../p9.c ../p9.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_p9.c ../p9.c

clean:
	rm -f $(OUT)/test_p9

.PHONY: test clean
