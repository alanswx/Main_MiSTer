// Host-side test of the Quadra 800 SONIC path: mac_eth_q8.cpp (op-list DMA client, ISR ownership)
// under mac_sonic.cpp, against a C model of the core's DMA engine (rtl/sonic_mbx.sv) and a flat
// big-endian guest RAM. No MiSTer, no FPGA:
//   g++ -O1 -Wall -o /tmp/mac_q8_test mac_q8_test.cpp ../mac_eth_q8.cpp ../mac_sonic.cpp && /tmp/mac_q8_test

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../mac_eth.h"
#include "../mac_eth_q8.h"
#include "../mac_sonic.h"

static uint8_t  ram[1 << 20];
static uint8_t  xfer[ETH_Q8_XFER_SIZE];
static uint64_t ops[ETH_Q8_MAX_OPS], cmd, stat, isr_set, isr_ack;

// what the engine did, in order: one record per guest longword beat
static struct { char wr; uint32_t a; uint8_t be; } beat[8192];
static int nbeat, rpcs;

// The engine as the RTL builds it: per op, longword beats from (ga & ~3); the first and last
// write beats carry byte enables; op k's bytes start at the next 8-aligned XFER offset.
static int engine(uint8_t seq)
{
	int n = (int)(cmd >> 8) & 15;
	uint32_t xp = 0;
	rpcs++;
	if ((uint8_t)cmd != seq) return -1;
	for (int k = 0; k < n; k++)
	{
		int      dir  = (int)(ops[k] & 1);
		uint32_t len  = (uint32_t)(ops[k] >> 16) & 0xffff;
		uint32_t ga   = (uint32_t)(ops[k] >> 32);
		uint32_t lead = ga & 3, a = ga & ~3u, end = lead + len;
		uint32_t beats = (end + 3) >> 2;
		if (ga >> 27) return -1;   // the RTL only carries addr[26:2]
		for (uint32_t i = 0; i < beats; i++, a += 4, xp += 4)
		{
			uint8_t be = 0xF;
			if (i == 0) be &= (uint8_t)(0xF >> lead);
			if (i == beats - 1 && (end & 3)) be &= (uint8_t)(0xF << (4 - (end & 3)));
			if (a + 4 > sizeof ram) return -1;
			for (int j = 0; j < 4; j++)
			{
				if (dir) { if (be & (8 >> j)) ram[a + j] = xfer[xp + j]; }
				else xfer[xp + j] = ram[a + j];
			}
			if (nbeat < 8192) { beat[nbeat].wr = (char)dir; beat[nbeat].a = a; beat[nbeat].be = dir ? be : 0xF; nbeat++; }
		}
		xp = (xp + 7) & ~7u;
	}
	stat = seq;
	return 0;
}

static uint8_t last_tx[2048];
static int     last_tx_len, tx_count;
static int wire(const uint8_t *f, int n) { memcpy(last_tx, f, n); last_tx_len = n; tx_count++; return n; }

static const sonic_host_ops host = { q8_read_words, q8_write_words, q8_read_bytes, q8_write_bytes, wire };

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void     wl(uint32_t a, uint32_t v) { ram[a] = v >> 24; ram[a + 1] = v >> 16; ram[a + 2] = v >> 8; ram[a + 3] = v; }
static uint32_t rl(uint32_t a) { return ((uint32_t)ram[a] << 24) | (ram[a + 1] << 16) | (ram[a + 2] << 8) | ram[a + 3]; }

enum { CR = 0, DCR, RCR, TCR, IMR, ISR, UTDA, CTDA, URDA = 0x0d, CRDA, URRA = 0x14, RSA, REA, RRP, RWP,
       CDP = 0x26, CDC = 0x27 };

static void start(void)
{
	memset(ram, 0, sizeof ram); memset(xfer, 0, sizeof xfer);
	cmd = stat = isr_set = isr_ack = 0;
	q8_mailbox m = { xfer, ops, &cmd, &stat, &isr_set, &isr_ack, 0, engine };
	sonic_init(&host);
	sonic_set_addr_bits(32);
	sonic_set_isr_local(1);
	q8_init(&m);
	q8_isr_reset();
	nbeat = rpcs = 0;
}

static void test_backend(void)
{
	start();
	memset(ram + 0x1000, 0xEE, 64);
	const uint8_t src[7] = { 1, 2, 3, 4, 5, 6, 7 };
	CHECK(q8_write_bytes(0x1003, src, 7) == 0, "unaligned write queues");
	CHECK(rpcs == 0, "a write alone is deferred");
	uint8_t back[9];
	CHECK(q8_read_bytes(0x1002, back, 9) == 0, "read flushes");
	CHECK(rpcs == 1, "write + read ride one RPC (%d)", rpcs);
	CHECK(back[0] == 0xEE && !memcmp(back + 1, src, 7) && back[8] == 0xEE, "neighbours survive a partial-longword write");
	CHECK(beat[0].wr && beat[0].a == 0x1000 && beat[0].be == 0x1, "lead byte enable %X", beat[0].be);
	CHECK(beat[2].wr && beat[2].a == 0x1008 && beat[2].be == 0xC, "trail byte enable %X", beat[2].be);

	// nine writes: the ninth forces the first eight out, in order
	rpcs = nbeat = 0;
	for (uint32_t i = 0; i < 9; i++) { uint8_t b = (uint8_t)(0x10 + i); q8_write_bytes(0x2000 + 4 * i, &b, 1); }
	CHECK(rpcs == 1, "eight ops per RPC");
	CHECK(q8_flush() == 0 && rpcs == 2, "flush lands the rest");
	for (uint32_t i = 0; i < 9; i++) CHECK(ram[0x2000 + 4 * i] == 0x10 + i, "op %u landed", i);

	// word reads: read-ahead serves the neighbours, a write or q8_begin drops it
	wl(0x3000, 0x0000BEEF); wl(0x3004, 0x00001234);
	uint16_t w[2];
	rpcs = 0;
	CHECK(q8_read_words(0x3000, w, 1, 4) == 0 && w[0] == 0xBEEF, "DW word = low half");
	CHECK(q8_read_words(0x3004, w, 1, 4) == 0 && w[0] == 0x1234 && rpcs == 1, "read-ahead hit");
	wl(0x3004, 0x00005678);
	q8_begin();
	CHECK(q8_read_words(0x3004, w, 1, 4) == 0 && w[0] == 0x5678 && rpcs == 2, "q8_begin forgets the read-ahead");
	uint16_t v = 0xCAFE;
	q8_write_words(0x3008, &v, 1, 4);
	CHECK(q8_read_words(0x3008, w, 1, 4) == 0 && w[0] == 0xCAFE, "a write drops the read-ahead and lands first");
	CHECK(rl(0x3008) == 0x0000CAFE, "DW write zero-fills the upper half");

	uint8_t b = 0;
	CHECK(q8_read_bytes(0x08000000, &b, 1) < 0 && q8_write_bytes(0x07FFFFFF, src, 2) < 0, "past 128 MB is refused here");
	CHECK(q8_read_bytes(sizeof ram - 4, back, 4) == 0, "top of the fake RAM reads");
}

#define RRA 0x10000
#define RDA 0x11000
#define RBA 0x20000
#define TDA 0x12000
#define CAMD 0x13000

static void bring_up(void)
{
	sonic_reg_write(CR, 0);                  // leave reset
	sonic_reg_write(DCR, 0x0020);            // DW: 32-bit data path
	// CAM: one entry, 08:00:07:12:34:56
	wl(CAMD, 0); wl(CAMD + 4, 0x0008); wl(CAMD + 8, 0x1207); wl(CAMD + 12, 0x5634); wl(CAMD + 16, 1);
	sonic_reg_write(URRA, 0x0001);
	sonic_reg_write(CDP, CAMD & 0xffff); sonic_reg_write(CDC, 1);
	sonic_reg_write(CR, 0x0200);             // LCAM
	// RRA: two buffers of 0x800 words
	for (int i = 0; i < 2; i++)
	{
		wl(RRA + 16 * i, (RBA + 0x1000 * i) & 0xffff); wl(RRA + 16 * i + 4, (RBA + 0x1000 * i) >> 16);
		wl(RRA + 16 * i + 8, 0x0800); wl(RRA + 16 * i + 12, 0);
	}
	sonic_reg_write(RSA, RRA & 0xffff); sonic_reg_write(REA, (RRA + 64) & 0xffff);
	sonic_reg_write(RRP, RRA & 0xffff); sonic_reg_write(RWP, (RRA + 32) & 0xffff);
	sonic_reg_write(CR, 0x0100);             // RRRA
	// RDA: four descriptors in a ring, all free (in_use = 1), last one EOL
	for (int i = 0; i < 4; i++)
	{
		uint32_t d = RDA + 28 * i;
		wl(d + 20, ((RDA + 28 * ((i + 1) & 3)) & 0xffff) | (i == 3 ? 1 : 0));
		wl(d + 24, 1);
	}
	sonic_reg_write(URDA, RDA >> 16); sonic_reg_write(CRDA, RDA & 0xffff);
	sonic_reg_write(RCR, 0x2000);            // accept broadcast
	sonic_reg_write(IMR, 0x7fff);
	sonic_reg_write(CR, 0x0008);             // RXEN
	q8_flush();
}

static void test_rx(void)
{
	start(); bring_up();
	CHECK(sonic_take_raised() == 0x1000, "LCAM raised LCD as an event");

	uint8_t f[100];
	memset(f, 0xA5, sizeof f);
	const uint8_t me[6] = { 0x08, 0x00, 0x07, 0x12, 0x34, 0x56 };
	memcpy(f, me, 6);
	rpcs = nbeat = 0;
	q8_begin();
	CHECK(sonic_rx_frame(f, sizeof f) == 1, "unicast to the CAM entry is delivered");
	q8_flush();
	CHECK(rpcs == 2, "one frame = two RPCs (%d)", rpcs);
	CHECK(!memcmp(ram + RBA, f, sizeof f), "frame bytes in the receive buffer");
	CHECK(rl(RDA + 4) == sizeof f + 4, "byte count includes the FCS (%u)", rl(RDA + 4));
	CHECK(rl(RDA + 8) == (RBA & 0xffff) && rl(RDA + 12) == (RBA >> 16), "packet pointer");
	CHECK((rl(RDA) & 1) && !(rl(RDA) >> 16), "status PRX, upper half zero (%08X)", rl(RDA));
	CHECK(rl(RDA + 24) == 0, "in_use cleared");
	// ORDERING LAW: the status longword is the last guest write of the frame
	int last_w = -1;
	for (int i = 0; i < nbeat; i++) if (beat[i].wr) last_w = i;
	CHECK(last_w >= 0 && beat[last_w].a == RDA, "status is published last (last write %X)", last_w >= 0 ? beat[last_w].a : 0);
	for (int i = last_w + 1; i < nbeat; i++) CHECK(0, "a read follows the publish at %X", beat[i].a);
	CHECK(sonic_take_raised() == 0x0400, "PKTRX raised");

	// 32-bit mode keeps the buffer pointer longword-aligned: a 101-byte frame (105 with its FCS)
	// is padded to 108 with $FF, and the next frame starts there
	uint8_t odd[101];
	memset(odd, 0x3C, sizeof odd); memcpy(odd, me, 6);
	q8_begin(); CHECK(sonic_rx_frame(odd, sizeof odd) == 1, "odd-length frame delivered"); q8_flush();
	q8_begin(); CHECK(sonic_rx_frame(f, sizeof f) == 1, "frame after it delivered"); q8_flush();
	uint32_t d1 = RDA + 28, d2 = RDA + 56;
	CHECK(rl(d1 + 4) == 105, "byte count is the real length (%u)", rl(d1 + 4));
	CHECK(rl(d1 + 8) == ((RBA + 104) & 0xffff), "second frame starts after the first (%X)", rl(d1 + 8));
	CHECK(ram[RBA + 104 + 105] == 0xff && ram[RBA + 104 + 107] == 0xff, "padding is $FF");
	CHECK(rl(d2 + 8) == ((RBA + 104 + 108) & 0xffff), "third frame is longword-aligned (%X)", rl(d2 + 8));
	sonic_take_raised();

	uint8_t other[64] = { 0x02, 1, 2, 3, 4, 5 };
	rpcs = 0;
	q8_begin();
	CHECK(sonic_rx_frame(other, sizeof other) == 0 && rpcs == 0, "a foreign unicast costs no RPC");
}

static void test_tx(void)
{
	start(); bring_up();
	sonic_take_raised();
	// one packet, two fragments: 14 bytes at an odd address + 50 bytes
	uint8_t pkt[64];
	for (int i = 0; i < 64; i++) pkt[i] = (uint8_t)i;
	memcpy(ram + 0x30001, pkt, 14); memcpy(ram + 0x31000, pkt + 14, 50);
	wl(TDA, 0); wl(TDA + 4, 0); wl(TDA + 8, 64); wl(TDA + 12, 2);
	wl(TDA + 16, 0x0001); wl(TDA + 20, 0x0003); wl(TDA + 24, 14);
	wl(TDA + 28, 0x1000); wl(TDA + 32, 0x0003); wl(TDA + 36, 50);
	wl(TDA + 40, (TDA & 0xffff) | 1);   // link: EOL
	sonic_reg_write(UTDA, TDA >> 16); sonic_reg_write(CTDA, TDA & 0xffff);
	rpcs = nbeat = 0;
	q8_begin();
	sonic_reg_write(CR, 0x0002);         // TXP
	q8_flush();
	CHECK(tx_count == 1 && last_tx_len == 64 && !memcmp(last_tx, pkt, 64), "gathered frame on the wire (%d bytes)", last_tx_len);
	CHECK(rpcs <= 4, "descriptor + two fragments + status/link = four RPCs (%d)", rpcs);
	CHECK((rl(TDA) & 1) == 1, "PTX status written back (%08X)", rl(TDA));
	CHECK(!(sonic_reg(CR) & 2), "TXP cleared at end of list");
	CHECK(sonic_take_raised() & 0x0200, "TXDN raised");
}

static void test_isr(void)
{
	start();
	// post #1 carries PKTRX; the FPGA has not consumed it yet
	sonic_reg_write(CR, 0); sonic_reg_write(DCR, 0x0020);
	bring_up(); sonic_take_raised();
	uint16_t s0 = (uint16_t)isr_set;
	uint8_t f[64]; memset(f, 0xFF, sizeof f);
	q8_begin(); sonic_rx_frame(f, sizeof f); q8_flush();
	q8_isr_post(0x1234);
	CHECK((uint16_t)isr_set == (uint16_t)(s0 + 1) && ((isr_set >> 16) & 0x7fff) == 0x0400, "PKTRX posted (%llX)", (unsigned long long)isr_set);
	CHECK((isr_set >> 48) == 0x1234, "the post carries the applied index");

	// second raise while the post is in flight: held back
	q8_begin(); sonic_rx_frame(f, sizeof f); q8_flush();
	q8_isr_post(0x1234);
	CHECK((uint16_t)isr_set == (uint16_t)(s0 + 1), "one post in flight at a time");

	// a guest ack written before the FPGA consumed post #1 cannot clear the replica
	CHECK(q8_isr_qualify(0x0400, s0) == 0, "ack older than the raise is kept");
	// the FPGA consumes post #1; the held raise goes out as post #2
	isr_ack = (uint16_t)(s0 + 1);
	q8_isr_post(0x1234);
	CHECK((uint16_t)isr_set == (uint16_t)(s0 + 2), "held raise posted after the ack");
	CHECK(q8_isr_qualify(0x0400, (uint16_t)(s0 + 1)) == 0, "ack that saw post #1 but not #2 is kept");
	CHECK(q8_isr_qualify(0x0400, (uint16_t)(s0 + 2)) == 0x0400, "ack that saw post #2 clears");
	sonic_reg_write(ISR, 0x0400);
	CHECK(!(sonic_reg(ISR) & 0x0400), "replica clear");

	// a bit raised and not yet posted at all is never clearable
	q8_begin(); sonic_rx_frame(f, sizeof f); q8_flush();
	CHECK(q8_isr_qualify(0x0400, (uint16_t)(s0 + 2)) == 0, "unposted raise survives any ack");
}

int main(void)
{
	test_backend();
	test_rx();
	test_tx();
	test_isr();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
