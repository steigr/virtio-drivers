/*
 * Host test of driver/virtq.c: a fake device that reads and writes the rings
 * the way the virtio spec lays them out, by byte offset and little-endian,
 * so a mistake in the driver's layout or byte order cannot cancel itself out.
 *
 *   make -C tests/host
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtq.h"

#define NUM 8

static unsigned wb_calls, inv_calls, mb_calls;

static void count_wb(void *a, unsigned long n) { (void)a; (void)n; wb_calls++; }
static void count_inv(void *a, unsigned long n) { (void)a; (void)n; inv_calls++; }
static void count_mb(void) { mb_calls++; }

static const struct vq_sync_ops ops = { count_wb, count_inv, count_mb };

/* The device's side of one queue. */
struct dev {
	unsigned char desc[VQ_DESC_BYTES(NUM)];
	unsigned char avail[VQ_AVAIL_BYTES(NUM)];
	unsigned char used[VQ_USED_BYTES(NUM)];
	uint16_t last_avail;
	uint16_t used_idx;
};

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const unsigned char *p)
{
	return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static void wr16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static void wr32(unsigned char *p, uint32_t v)
{
	wr16(p, (uint16_t)v);
	wr16(p + 2, (uint16_t)(v >> 16));
}

/* Next chain the driver published: its head, address, length and flags. */
static int dev_pop(struct dev *d, uint16_t *head, uint64_t *addr, uint32_t *len, uint16_t *flags)
{
	if (rd16(d->avail + 2) == d->last_avail)
		return 0;
	*head = rd16(d->avail + 4 + 2 * (d->last_avail % NUM));
	d->last_avail++;
	assert(*head < NUM);
	*addr = rd64(d->desc + 16 * *head);
	*len = rd32(d->desc + 16 * *head + 8);
	*flags = rd16(d->desc + 16 * *head + 12);
	return 1;
}

static void dev_used(struct dev *d, uint16_t head, uint32_t written)
{
	unsigned char *e = d->used + 4 + 8 * (d->used_idx % NUM);

	wr32(e, head);
	wr32(e + 4, written);
	d->used_idx++;
	wr16(d->used + 2, d->used_idx);
}

static void setup(struct virtq *vq, struct dev *d, void **cookies)
{
	memset(d, 0, sizeof(*d));
	assert(virtq_init(vq, NUM, d->desc, d->avail, d->used, cookies, &ops) == 0);
}

static void test_bad_size(void)
{
	struct virtq vq;
	struct dev d;
	void *cookies[NUM];

	memset(&d, 0, sizeof(d));
	assert(virtq_init(&vq, 0, d.desc, d.avail, d.used, cookies, 0) == -1);
	assert(virtq_init(&vq, 6, d.desc, d.avail, d.used, cookies, 0) == -1);
}

static void test_layout_is_little_endian(void)
{
	struct virtq vq;
	struct dev d;
	void *cookies[NUM];
	int token;

	setup(&vq, &d, cookies);
	assert(virtq_add(&vq, 0x1122334455667788ULL, 0xAABBCCDD, 1, &token) == 0);
	/* Bytes, exactly as the spec's little-endian layout has them. */
	assert(d.desc[0] == 0x88 && d.desc[7] == 0x11);
	assert(d.desc[8] == 0xDD && d.desc[11] == 0xAA);
	assert(rd16(d.desc + 12) == VRING_DESC_F_WRITE);
	/* Not published yet. */
	assert(rd16(d.avail + 2) == 0);
	assert(virtq_publish(&vq) == 1);
	assert(rd16(d.avail + 2) == 1);
	assert(rd16(d.avail + 4) == 0);
	assert(virtq_publish(&vq) == 0);
}

static void test_round_trip_and_wrap(void)
{
	struct virtq vq;
	struct dev d;
	void *cookies[NUM];
	int tokens[NUM * 8];
	unsigned i;

	setup(&vq, &d, cookies);
	/* Far past the ring size, so avail, used and the free list all wrap. */
	for (i = 0; i < NUM * 8; i++) {
		uint16_t head, flags;
		uint64_t addr;
		uint32_t len, written = 0;

		assert(virtq_add(&vq, 0x1000 + i, 16 + i, i & 1, &tokens[i]) == 0);
		assert(virtq_publish(&vq) == 1);
		assert(dev_pop(&d, &head, &addr, &len, &flags));
		assert(addr == 0x1000 + i && len == 16 + i);
		assert(flags == ((i & 1) ? VRING_DESC_F_WRITE : 0));
		dev_used(&d, head, i);
		assert(virtq_get_used(&vq, &written) == &tokens[i]);
		assert(written == i);
		assert(virtq_get_used(&vq, &written) == NULL);
	}
	assert(vq.num_free == NUM);
}

static void test_full_ring_and_out_of_order(void)
{
	struct virtq vq;
	struct dev d;
	void *cookies[NUM];
	int tokens[NUM], extra;
	uint16_t heads[NUM], flags;
	uint64_t addr;
	uint32_t len;
	int i, seen[NUM];

	setup(&vq, &d, cookies);
	for (i = 0; i < NUM; i++)
		assert(virtq_add(&vq, 0x2000 + i, 8, 0, &tokens[i]) == 0);
	assert(virtq_add(&vq, 0x9000, 8, 0, &extra) == -1);
	assert(virtq_publish(&vq) == NUM);
	for (i = 0; i < NUM; i++)
		assert(dev_pop(&d, &heads[i], &addr, &len, &flags));
	/* Completed backwards: every descriptor comes back free and reusable. */
	memset(seen, 0, sizeof(seen));
	for (i = NUM - 1; i >= 0; i--)
		dev_used(&d, heads[i], 0);
	for (i = 0; i < NUM; i++) {
		int *t = virtq_get_used(&vq, &len);

		assert(t == &tokens[NUM - 1 - i]);
		seen[t - tokens]++;
	}
	for (i = 0; i < NUM; i++)
		assert(seen[i] == 1);
	assert(vq.num_free == NUM);
	for (i = 0; i < NUM; i++)
		assert(virtq_add(&vq, 0x3000 + i, 8, 0, &tokens[i]) == 0);
	assert(virtq_add(&vq, 0x9000, 8, 0, &extra) == -1);
}

static void test_batched_add_wraps_the_avail_ring(void)
{
	struct virtq vq;
	struct dev d;
	void *cookies[NUM];
	int tokens[NUM];
	uint16_t head, flags;
	uint64_t addr;
	uint32_t len;
	int i;

	setup(&vq, &d, cookies);
	/* Move the avail index near the end of the ring. */
	for (i = 0; i < NUM - 2; i++) {
		assert(virtq_add(&vq, 1, 1, 0, &tokens[0]) == 0);
		virtq_publish(&vq);
		assert(dev_pop(&d, &head, &addr, &len, &flags));
		dev_used(&d, head, 0);
		assert(virtq_get_used(&vq, &len) == &tokens[0]);
	}
	for (i = 0; i < 4; i++)
		assert(virtq_add(&vq, 0x4000 + i, 1, 0, &tokens[i]) == 0);
	assert(virtq_publish(&vq) == 4);
	for (i = 0; i < 4; i++) {
		assert(dev_pop(&d, &head, &addr, &len, &flags));
		assert(addr == (uint64_t)0x4000 + i);
	}
}

static void test_bogus_used_entry_is_skipped(void)
{
	struct virtq vq;
	struct dev d;
	void *cookies[NUM];
	int token;
	uint16_t head, flags;
	uint64_t addr;
	uint32_t len;

	setup(&vq, &d, cookies);
	assert(virtq_add(&vq, 1, 1, 0, &token) == 0);
	virtq_publish(&vq);
	assert(dev_pop(&d, &head, &addr, &len, &flags));
	dev_used(&d, 200, 0);	/* out of range */
	dev_used(&d, 3, 0);	/* in range, never handed out */
	dev_used(&d, head, 7);
	assert(virtq_get_used(&vq, &len) == &token);
	assert(len == 7);
	assert(virtq_get_used(&vq, &len) == NULL);
}

static void test_no_interrupt_flag(void)
{
	struct virtq vq;
	struct dev d;
	void *cookies[NUM];

	setup(&vq, &d, cookies);
	virtq_set_no_interrupt(&vq, 1);
	assert(rd16(d.avail) == VRING_AVAIL_F_NO_INTERRUPT);
	virtq_set_no_interrupt(&vq, 0);
	assert(rd16(d.avail) == 0);
}

static void test_sync_ops_run_where_the_device_needs_them(void)
{
	struct virtq vq;
	struct dev d;
	void *cookies[NUM];
	int token;
	unsigned wb, mb;
	uint32_t len;

	setup(&vq, &d, cookies);
	wb = wb_calls;
	assert(virtq_add(&vq, 1, 1, 0, &token) == 0);
	assert(wb_calls == wb + 1);	/* the descriptor */
	wb = wb_calls;
	mb = mb_calls;
	virtq_publish(&vq);
	assert(wb_calls == wb + 2);	/* the ring, then the index */
	assert(mb_calls >= mb + 2);
	inv_calls = 0;
	assert(virtq_get_used(&vq, &len) == NULL);
	assert(inv_calls == 1);		/* the used ring, before it is read */
}

int main(void)
{
	test_bad_size();
	test_layout_is_little_endian();
	test_round_trip_and_wrap();
	test_full_ring_and_out_of_order();
	test_batched_add_wraps_the_avail_ring();
	test_bogus_used_entry_is_skipped();
	test_no_interrupt_flag();
	test_sync_ops_run_where_the_device_needs_them();
	puts("test_virtq: ok");
	return 0;
}
