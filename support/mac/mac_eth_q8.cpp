// Quadra 800 SONIC: op-list DMA client. One RPC = up to 8 ordered guest-RAM transfers.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "mac_eth.h"
#include "mac_eth_q8.h"
#include "mac_sonic.h"

q8_stats_t q8_stats;

static q8_mailbox mbx;
static uint8_t    seq;
// The engine reads its ops and the XFER window on demand: a list it has not finished is never overwritten.
static int        stale;

// Queued ops: each op's bytes sit at an 8-aligned XFER offset, guest byte (ga & ~3) + j at xoff + j.
static struct
{
	uint32_t ga, len, xoff;
	int      wr;
	uint8_t *dst;              // read destination, filled after the engine ran
} q[ETH_Q8_MAX_OPS];
static int      nq;
static uint32_t xnext;

// Descriptor read-ahead: one fetched block serves the word reads that follow it.
#define AHEAD_BYTES 256
static uint8_t  ahead[AHEAD_BYTES];
static uint32_t ahead_ga, ahead_len;

static inline uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

void q8_init(const q8_mailbox *m)
{
	mbx = *m;
	seq = (uint8_t)*mbx.cmd;   // the engine adopts the staged seq as done at first sight
	stale = 0;
	nq = 0;
	xnext = 0;
	ahead_len = 0;
}

void q8_begin(void) { ahead_len = 0; }

static uint32_t span_of(uint32_t ga, uint32_t len) { return ((ga & 3) + len + 3) & ~3u; }

#define SPIN_US 1500   // usleep() costs ~1 ms; the engine answers in tens of microseconds
static int run(void)
{
	if (!nq) return 0;
	if (stale)
	{
		uint64_t t0 = now_us();
		while ((uint8_t)*mbx.stat != seq && now_us() - t0 < 2000000)
		{
			if (mbx.idle) mbx.idle();
			usleep(50);
		}
		if ((uint8_t)*mbx.stat != seq)
		{
			q8_stats.rpc_fail++;
			nq = 0;
			xnext = 0;
			return -1;
		}
		stale = 0;
	}
	for (int i = 0; i < nq; i++)
		mbx.ops[i] = ((uint64_t)q[i].ga << 32) | ((uint64_t)q[i].len << 16) | (q[i].wr ? 1 : 0);
	if (++seq == 0) seq = 1;   // 0 = the engine's reset state
	__sync_synchronize();
	*mbx.cmd = ((uint64_t)nq << 8) | seq;
	__sync_synchronize();

	int rc = -1, slept = 0;
	uint64_t t0 = now_us(), el = 0;
	if (mbx.wait) rc = mbx.wait(seq);
	else for (;;)
	{
		if ((uint8_t)*mbx.stat == seq) { rc = 0; break; }
		// A backed-up doorbell ring stalls the guest bus the engine needs.
		if (mbx.idle) mbx.idle();
		el = now_us() - t0;
		if (el > 250000)
		{
			printf("mac_eth: DMA timeout (seq %u, %d ops, first %08X+%u)\n", seq, nq, q[0].ga, q[0].len);
			stale = !mbx.wait;
			break;
		}
		if (el > SPIN_US) { usleep(50); slept = 1; }
	}
	el = now_us() - t0;
	q8_stats.rpc++; q8_stats.ops += nq; q8_stats.rpc_us += el;
	if (slept) q8_stats.rpc_slept++;
	if (el > q8_stats.rpc_us_max) q8_stats.rpc_us_max = el;
	if (rc) q8_stats.rpc_fail++;
	else for (int i = 0; i < nq; i++)
		if (!q[i].wr && q[i].dst)
			memcpy(q[i].dst, (const void *)(mbx.xfer + q[i].xoff + (q[i].ga & 3)), q[i].len);
	nq = 0;
	xnext = 0;
	return rc;
}

int q8_flush(void) { return run(); }

// The engine carries addr[26:2]: anything past 128 MB would wrap into real RAM.
static int bad(uint32_t ga, uint32_t len)
{
	if (len && ga < ETH_Q8_RAM_TOP && len <= ETH_Q8_RAM_TOP - ga && span_of(ga, len) <= ETH_Q8_XFER_SIZE)
		return 0;
	q8_stats.bad_addr++;
	return 1;
}

static int enqueue(uint32_t ga, uint32_t len, int wr, const uint8_t *src, uint8_t *dst)
{
	uint32_t span = span_of(ga, len);
	if (nq == ETH_Q8_MAX_OPS || xnext + span > ETH_Q8_XFER_SIZE)
		if (run()) return -1;
	q[nq].ga = ga; q[nq].len = len; q[nq].xoff = xnext; q[nq].wr = wr; q[nq].dst = dst;
	if (wr) memcpy((void *)(mbx.xfer + xnext + (ga & 3)), src, len);
	nq++;
	xnext = (xnext + span + 7) & ~7u;
	return 0;
}

static int drop_writes;
void q8_drop_writes(int on) { drop_writes = on; }

static int wr(uint32_t ga, const uint8_t *src, uint32_t len)
{
	if (bad(ga, len)) return -1;
	if (drop_writes) return 0;
	ahead_len = 0;
	return enqueue(ga, len, 1, src, 0);
}

static int rd(uint32_t ga, uint8_t *dst, uint32_t len, int want_ahead)
{
	if (ahead_len && ga >= ahead_ga && ga - ahead_ga + len <= ahead_len)
	{
		memcpy(dst, ahead + (ga - ahead_ga), len);
		q8_stats.ahead_hit++;
		return 0;
	}
	if (bad(ga, len)) return -1;
	if (want_ahead && len <= AHEAD_BYTES)
	{
		uint32_t n = AHEAD_BYTES;
		if (n > ETH_Q8_RAM_TOP - ga) n = ETH_Q8_RAM_TOP - ga;
		ahead_len = 0;
		if (enqueue(ga, n, 0, 0, ahead) || run()) return -1;
		ahead_ga = ga; ahead_len = n;
		memcpy(dst, ahead, len);
		return 0;
	}
	if (enqueue(ga, len, 0, 0, dst)) return -1;
	return run();
}

// 16-bit big-endian fields; stride 4 = DCR.DW longword mode, value in the low half, upper half zero.
int q8_read_words(uint32_t ga, uint16_t *v, int n, int stride)
{
	uint8_t b[64 * 4];
	if (n <= 0 || n > 64) return -1;
	if (rd(ga, b, (uint32_t)n * stride, 1)) return -1;
	for (int i = 0; i < n; i++)
	{
		const uint8_t *p = b + i * stride + (stride == 4 ? 2 : 0);
		v[i] = (uint16_t)((p[0] << 8) | p[1]);
	}
	return 0;
}

int q8_write_words(uint32_t ga, const uint16_t *v, int n, int stride)
{
	uint8_t b[64 * 4];
	if (n <= 0 || n > 64) return -1;
	memset(b, 0, (size_t)n * stride);
	for (int i = 0; i < n; i++)
	{
		uint8_t *p = b + i * stride + (stride == 4 ? 2 : 0);
		p[0] = (uint8_t)(v[i] >> 8);
		p[1] = (uint8_t)v[i];
	}
	return wr(ga, b, (uint32_t)n * stride);
}

int q8_read_bytes(uint32_t ga, uint8_t *b, int n)  { return n > 0 ? rd(ga, b, (uint32_t)n, 0) : 0; }
int q8_write_bytes(uint32_t ga, const uint8_t *b, int n) { return n > 0 ? wr(ga, b, (uint32_t)n) : 0; }

// ---- ISR ownership ---------------------------------------------------------------------------

static uint16_t isr_seq;          // last seq written to ISR_SET
static uint16_t isr_unposted;     // raised, waiting for the post in flight to be consumed
static uint16_t isr_bit_seq[16];  // the post that carried each bit's latest raise

void q8_isr_reset(void)
{
	isr_seq = (uint16_t)*mbx.isr_set;   // the FPGA adopts the staged seq at first sight
	isr_unposted = 0;
	for (int b = 0; b < 16; b++) isr_bit_seq[b] = isr_seq;
	sonic_take_raised();
}

// `applied` rides in the same 64-bit word: the FPGA drops its CR overlay in the clock it raises the
// bits, as the chip clears TXP in the instant it sets TXDN (the driver reads CR in that handler).
void q8_isr_post(uint16_t applied)
{
	isr_unposted |= sonic_take_raised();
	q8_stats.isr_seq = isr_seq; q8_stats.isr_unposted = isr_unposted;
	if (!isr_unposted || (uint16_t)*mbx.isr_ack != isr_seq) return;
	isr_seq++;
	for (int b = 0; b < 16; b++)
	{
		if (isr_unposted & (1 << b)) isr_bit_seq[b] = isr_seq;
		// keep a never-acked bit's stamp inside the signed compare window
		else if ((int16_t)(isr_seq - isr_bit_seq[b]) > 0x3000) isr_bit_seq[b] = (uint16_t)(isr_seq - 0x3000);
	}
	*mbx.isr_set = ((uint64_t)applied << 48) | ((uint64_t)isr_unposted << 16) | isr_seq;
	__sync_synchronize();
	isr_unposted = 0;
	q8_stats.isr_posts++;
}

// An ack clears a replica bit only if the FPGA had that bit's latest raise when the guest wrote:
// a raise it had not consumed yet sets the bit again after the guest's clear.
uint16_t q8_isr_qualify(uint16_t data, uint16_t seen)
{
	uint16_t ok = 0;
	isr_unposted |= sonic_take_raised();
	for (int b = 0; b < 15; b++)
	{
		if (!(data & (1 << b))) continue;
		if (!(isr_unposted & (1 << b)) && (int16_t)(seen - isr_bit_seq[b]) >= 0) ok |= (uint16_t)(1 << b);
		else q8_stats.isr_acks_kept++;
	}
	return ok;
}
