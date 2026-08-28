// Mac Ethernet service — Main side of the Apple Ethernet cards (mac_eth.h): LC PDS and NuBus TP.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#include "../../user_io.h"
#include "../../menu.h"
#include "../../shmem.h"
#include "../../hardware.h"
#include "mac.h"
#include "mac_eth.h"
#include "mac_sonic.h"
#include "mac_eth_declrom.h"

// Card personality: LC bus-masters guest RAM via the DMA-RPC engine, NB owns 128K card RAM in the window.
enum { CARD_LC, CARD_NB };
static int card_kind;

static volatile uint8_t *win;   // the mapped DDR3 window
static uint32_t ctrl_base;      // active control block base (per-card layout)
static int      card_up;
static uint32_t rptr;
static uint8_t  dma_seq;

static inline uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

// Throughput counters; the packet path only counts, /tmp/mac_eth_stats is written on the 1 s tick.
static struct
{
	uint64_t rpc, rpc_slept, rpc_fail, rpc_us, rpc_us_max;
	uint64_t rx_frames, rx_bytes, tx_frames, tx_bytes, drops, rx_refused;
	uint64_t txp_cmds;          // CR writes carrying TXP (transmit asked for)
	uint32_t ring_max, ring_ovf;// doorbell backlog watermark / overwrites
	uint32_t reg_wr[64];        // what the driver actually writes
	uint64_t tx_fail;           // wire_send failures (were silently ignored)
	uint64_t rx_ours, rx_ours_bytes;  // frames actually addressed to the guest
	uint64_t rx_arp, tx_arp;    // ethertype 0x0806 each way
	uint64_t rx_ip,  tx_ip;     // ethertype 0x0800 each way
	uint64_t rx_held, rx_held_max;    // unicasts held for redelivery / depth watermark
	uint64_t rx_jumbo;          // >1518-byte frames off the tap: offload leak witness
	uint64_t drain_full;        // drain budget exhausted with stash left: flood witness
} st;
static uint8_t  guest_mac[6];
static char     ifname[64] = "eth0";

static volatile uint64_t *w64(uint32_t off)
{
	return (volatile uint64_t *)(win + off);
}

// Control-block words are per-card (the two layouts share one internal shape).
static volatile uint64_t *ctl(uint32_t rel)
{
	return w64(ctrl_base + rel);
}

// Iface and MAC suffix live in Main-owned status bits, a2065-style: the FPGA never reads them.
#define ETH_OPT_IFACE  "[37:36]"   // core: "o45,Net interface,eth0,tap0,macvlan,eth1"
#define ETH_OPT_MACSUF "[35:32]"   // "o03,MAC suffix,0..F"; Main-only, clear of the core's [31:0]

static const char *const iface_names[4] = { "eth0", "tap0", "macvlan", "eth1" };

// Fixed base + OSD nibble: all selections stay unicast, and nothing on a stock DE10-Nano is unique.
#define MAC_OUI_0   0x08
#define MAC_OUI_1   0x00
#define MAC_OUI_2   0x07
#define MAC_FAMILY  0x4D           // 'M'
#define MAC_CORE_LC 0x4C           // 'L' - MacLC; every Mac core needs its own byte
#define MAC_CORE_NB 0x56           // 'V' - MacIIvi

// Slot address lines: 24 on the LC PDS, 32 on NuBus; a wrong value DMAs to the wrong address.
#define ADDR_BITS_DEFAULT 24
#define DECLROM_NAME      "ethernet.rom"
static int addr_bits = ADDR_BITS_DEFAULT;

// Exact match: the prefix-matching is_core_named() would also fire for the MacLCII core.
static int core_is_maclc(void)
{
	return !strcasecmp(user_io_get_core_name(0), "maclc")
	    || !strcasecmp(user_io_get_core_name(1), "maclc");
}

// NuBus-card cores, exact-matched for the same reason.
static int core_is_maciivi(void)
{
	return !strcasecmp(user_io_get_core_name(0), "maciivi")
	    || !strcasecmp(user_io_get_core_name(1), "maciivi");
}

// Optional <HomeDir>/eth.cfg overrides the OSD and the per-core defaults.
static void load_config(void)
{
	guest_mac[0] = MAC_OUI_0; guest_mac[1] = MAC_OUI_1; guest_mac[2] = MAC_OUI_2;
	guest_mac[3] = MAC_FAMILY;
	guest_mac[4] = (card_kind == CARD_NB) ? MAC_CORE_NB : MAC_CORE_LC;
	guest_mac[5] = (uint8_t)(user_io_status_get(ETH_OPT_MACSUF) & 0xF);
	addr_bits = ADDR_BITS_DEFAULT;

	// HomeDir() is relative to the storage root, so it only opens through getFullPath().
	FILE *f = fopen(getFullPath(user_io_make_filepath(HomeDir(), "eth.cfg")), "r");
	if (!f) return;
	char line[160];
	while (fgets(line, sizeof line, f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!strncasecmp(line, "iface=", 6) && line[6])
			snprintf(ifname, sizeof ifname, "%s", line + 6);
		else if (!strncasecmp(line, "mac=", 4))
		{
			unsigned m[6];
			if (sscanf(line + 4, "%x:%x:%x:%x:%x:%x",
			           &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6)
				for (int i = 0; i < 6; i++) guest_mac[i] = (uint8_t)m[i];
		}
		else if (!strncasecmp(line, "macbyte=", 8))
		{
			unsigned b;
			if (sscanf(line + 8, "%x", &b) == 1) guest_mac[4] = (uint8_t)b;
		}
		else if (!strncasecmp(line, "addrbits=", 9))
		{
			int b = atoi(line + 9);
			if (b == 24 || b == 32) addr_bits = b;
			else printf("mac_eth: addrbits=%s ignored (24 or 32)\n", line + 9);
		}
	}
	fclose(f);
}

// DMA-RPC client: the LC SONIC's guest-memory backend.
#define DMA_SPIN_US 1500   // busy-wait budget before falling back to usleep

static void ring_slurp(void);   // defined with the doorbell ring below

// SONIC register indices + bits we watch (values from mac_sonic.cpp).
#define SONIC_CR        0x00
#define SONIC_CR_TXP    0x0002
#define SONIC_IMR       0x04
#define SONIC_ISR       0x05
#define SONIC_CRDA      0x0E
#define SONIC_RRP       0x17
#define SONIC_RWP       0x18
#define SONIC_CR_RXEN   0x0008
#define SONIC_ISR_RXEXH 0x0060   // RBE | RDE: reception halted until cleared
static int dma_rpc(uint32_t gaddr, uint32_t len, int wr)
{
	if (!card_up) return -1;
	if (++dma_seq == 0) dma_seq = 1;   // 0 = the engine's reset state

	// addr is bits [39:16]: a dirty top byte would overlap COUNT (47:40) and inflate the transfer.
	if (gaddr > 0x00ffffffu)
	{
		// A wider slot can address past the field, where truncating would move the wrong memory.
		if (addr_bits > 24)
		{
			static int warned;
			if (!warned++)
				printf("mac_eth: guest address %08X exceeds the mailbox's 24-bit field\n", gaddr);
			st.rpc_fail++;
			return -1;
		}
		gaddr &= 0x00ffffffu;
	}

	*ctl(ETH_CTL_DMACMD) = ((uint64_t)len << 40) | ((uint64_t)gaddr << 16)
	                     | ((uint64_t)(wr ? 1 : 0) << 8) | dma_seq;
	__sync_synchronize();

	// Spin first: usleep() costs a ~1 ms round trip, far past the FPGA's ~32 us hot answer.
	uint64_t t0 = now_us(), el = 0;
	int rc = -1, slept = 0;
	for (;;)
	{
		uint64_t s = *ctl(ETH_CTL_DMASTAT);
		if ((s & 0xff) == dma_seq) { rc = (s & 0x100) ? -1 : 0; break; }
		// Stash-only drain: a backed-up ring throttles the guest bus, which blocks this very RPC.
		ring_slurp();
		el = now_us() - t0;
		// The engine can need tens of ms; a short deadline aborts the transmit and wedges TCP.
		if (el > 250000)
		{
			printf("mac_eth: DMA-RPC timeout (seq %u addr 0x%x len %u)\n",
			       dma_seq, gaddr, len);
			break;
		}
		if (el > DMA_SPIN_US) { usleep(50); slept = 1; }
	}
	el = now_us() - t0;
	st.rpc++; st.rpc_us += el; if (slept) st.rpc_slept++;
	if (rc) st.rpc_fail++;
	if (el > st.rpc_us_max) st.rpc_us_max = el;
	return rc;
}

// The DMA engine moves even-addressed words, the SONIC uses odd ones: align here, never reject.
static int rpc_read(uint32_t ga, uint8_t *dst, uint32_t len)
{
	if (!len) return 0;
	uint32_t a0  = ga & ~1u;              // word-aligned start
	uint32_t off = ga - a0;               // 0 or 1
	uint32_t n   = (off + len + 1) & ~1u; // even byte count spanning [ga, ga+len)
	if (n > 0xFFFE) return -1;
	if (dma_rpc(a0, n, 0)) return -1;
	memcpy(dst, (const void *)(win + ETH_LC_OFF_XFER + off), len);
	return 0;
}

static int rpc_write(uint32_t ga, const uint8_t *src, uint32_t len)
{
	if (!len) return 0;
	uint32_t a0  = ga & ~1u;
	uint32_t off = ga - a0;
	uint32_t n   = (off + len + 1) & ~1u;
	if (n > 0xFFFE) return -1;
	// Read-modify-write on an unaligned edge: the head/tail bytes sharing a word must survive.
	if (off || (n != off + len))
	{
		if (dma_rpc(a0, n, 0)) return -1;   // prime XFER with the current words
	}
	memcpy((void *)(win + ETH_LC_OFF_XFER + off), src, len);
	return dma_rpc(a0, n, 1);
}

// Grouped 68k big-endian word access: stride 2 = the LC's 16-bit bus, stride 4 = DCR_DW dword mode.
static int gw_read_words(uint32_t ga, uint16_t *v, int n, int stride)
{
	uint8_t b[64 * 4];
	if (n <= 0 || n > 64) return -1;
	if (rpc_read(ga, b, (uint32_t)n * stride)) return -1;
	for (int i = 0; i < n; i++)
	{
		const uint8_t *p = b + i * stride + (stride == 4 ? 2 : 0);
		v[i] = (uint16_t)((p[0] << 8) | p[1]);
	}
	return 0;
}

static int gw_write_words(uint32_t ga, const uint16_t *v, int n, int stride)
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
	return rpc_write(ga, b, (uint32_t)n * stride);
}

static int gb_read(uint32_t ga, uint8_t *b, int n)  { return rpc_read(ga, b, (uint32_t)n); }
static int gb_write(uint32_t ga, const uint8_t *b, int n) { return rpc_write(ga, b, (uint32_t)n); }
static int wire_send(const uint8_t *f, int n)
{
	st.tx_frames++; st.tx_bytes += n;
	if (n >= 14)
	{
		unsigned et = ((unsigned)f[12] << 8) | f[13];
		if (et == 0x0806) st.tx_arp++;
		else if (et == 0x0800) st.tx_ip++;
	}
	int rc = mac_eth_iface_send(f, n);
	// A short write means the frame never reached the wire; do not count it as sent.
	if (rc != n) st.tx_fail++;
	return rc;
}

static const sonic_host_ops lc_host_ops = {
	gw_read_words, gw_write_words, gb_read, gb_write, wire_send
};

// NB backend: the card's space maps its RAM at slot_base, so the low 17 bits of any SONIC address land in the window.
#define NB_RAM_MASK (ETH_NB_RAM_SIZE - 1)

static int nb_read_words(uint32_t ga, uint16_t *v, int n, int stride)
{
	volatile uint8_t *ram = win + ETH_NB_OFF_RAM;
	if (n <= 0 || n > 64) return -1;
	for (int i = 0; i < n; i++)
	{
		uint32_t a = (ga + (uint32_t)i * stride + (stride == 4 ? 2 : 0)) & NB_RAM_MASK;
		v[i] = (uint16_t)((ram[a] << 8) | ram[(a + 1) & NB_RAM_MASK]);
	}
	return 0;
}

static int nb_write_words(uint32_t ga, const uint16_t *v, int n, int stride)
{
	volatile uint8_t *ram = win + ETH_NB_OFF_RAM;
	if (n <= 0 || n > 64) return -1;
	for (int i = 0; i < n; i++)
	{
		uint32_t a = (ga + (uint32_t)i * stride + (stride == 4 ? 2 : 0)) & NB_RAM_MASK;
		ram[a] = (uint8_t)(v[i] >> 8);
		ram[(a + 1) & NB_RAM_MASK] = (uint8_t)v[i];
	}
	return 0;
}

static int nb_read_bytes(uint32_t ga, uint8_t *b, int n)
{
	volatile uint8_t *ram = win + ETH_NB_OFF_RAM;
	for (int i = 0; i < n; i++) b[i] = ram[(ga + (uint32_t)i) & NB_RAM_MASK];
	return 0;
}

static int nb_write_bytes(uint32_t ga, const uint8_t *b, int n)
{
	volatile uint8_t *ram = win + ETH_NB_OFF_RAM;
	for (int i = 0; i < n; i++) ram[(ga + (uint32_t)i) & NB_RAM_MASK] = b[i];
	return 0;
}

static const sonic_host_ops nb_host_ops = {
	nb_read_words, nb_write_words, nb_read_bytes, nb_write_bytes, wire_send
};

// Apple declaration ROM CRC: rotate-left-1 then add, with the stored CRC read as zero.
static uint32_t declrom_crc(const uint8_t *p, uint32_t n, uint32_t crc_off)
{
	uint32_t s = 0;
	for (uint32_t i = 0; i < n; i++)
	{
		uint32_t b = (i >= crc_off && i < crc_off + 4) ? 0 : p[i];
		s = ((s << 1) | (s >> 31)) + b;
	}
	return s;
}

// Format block at the image tail; validates any Apple declROM against the card's byteLanes.
static int declrom_valid(const uint8_t *d, uint32_t size, int quiet, uint8_t want_lanes)
{
	if (size < 20) return 0;
	uint32_t length = ((uint32_t)d[size - 16] << 24) | ((uint32_t)d[size - 15] << 16)
	                | ((uint32_t)d[size - 14] << 8)  | d[size - 13];
	uint32_t crc    = ((uint32_t)d[size - 12] << 24) | ((uint32_t)d[size - 11] << 16)
	                | ((uint32_t)d[size - 10] << 8)  | d[size - 9];
	uint32_t testpat = ((uint32_t)d[size - 6] << 24) | ((uint32_t)d[size - 5] << 16)
	                 | ((uint32_t)d[size - 4] << 8)  | d[size - 3];
	uint8_t bytelanes = d[size - 1];
	if (testpat != 0x5A932BC7 || length > size || length < 20)
	{
		if (!quiet) printf("mac_eth: declROM format block bad (testPattern %08X)\n",
		                   testpat);
		return 0;
	}
	if (bytelanes != want_lanes)
	{
		if (!quiet) printf("mac_eth: declROM byteLanes %02X, this card needs %02X\n",
		                   bytelanes, want_lanes);
		return 0;
	}
	const uint8_t *span = d + size - length;
	uint32_t computed = declrom_crc(span, length, length - 12);
	if (computed != crc)
	{
		if (!quiet) printf("mac_eth: declROM CRC %08X != stored %08X\n", computed, crc);
		return 0;
	}
	return 1;
}

// The ROM is a per-core asset: <HomeDir>/ethernet.rom, or the built-in image on the MacLC.
static uint8_t  declrom[MAC_ETH_DECLROM_SIZE];
static uint32_t declrom_len;

static int load_declrom(int quiet)
{
	// LC ROM 341-0740 is flat (byteLanes $0F); the NB ROM 341-1096 is lane 1 only ($D2).
	uint8_t want_lanes = (card_kind == CARD_NB) ? 0xD2 : 0x0F;
	declrom_len = 0;

	char path[1200];
	snprintf(path, sizeof path, "%s", getFullPath(user_io_make_filepath(HomeDir(), DECLROM_NAME)));
	FILE *f = fopen(path, "rb");
	if (f)
	{
		size_t n = fread(declrom, 1, sizeof declrom, f);
		int over = (fgetc(f) != EOF);   // filling the buffer exactly does not set EOF
		fclose(f);
		if (over || !n)
		{
			if (!quiet) printf("mac_eth: %s is not a %u-byte declROM\n",
			                   path, (unsigned)sizeof declrom);
			return 0;
		}
		if (!declrom_valid(declrom, (uint32_t)n, quiet, want_lanes))
		{
			if (!quiet) printf("mac_eth: %s failed validation - card stays down\n", path);
			return 0;
		}
		declrom_len = (uint32_t)n;
		if (!quiet) printf("mac_eth: declROM %s (%u bytes)\n", path, declrom_len);
		return 1;
	}

	if (!core_is_maclc()) return 0;   // any other core must supply its own

	const uint8_t *d = mac_eth_declrom_data;
	unsigned nspans = sizeof mac_eth_declrom_spans / sizeof mac_eth_declrom_spans[0];
	memset(declrom, 0, sizeof declrom);
	for (unsigned s = 0; s < nspans; s++)
		for (uint16_t i = 0; i < mac_eth_declrom_spans[s].len; i++)
			declrom[mac_eth_declrom_spans[s].off + i] = *d++;
	declrom_len = MAC_ETH_DECLROM_SIZE;
	return 1;
}

static void stage_declrom(void)
{
	if (card_kind == CARD_NB)
	{
		// RAW image right-aligned in the 32K ROMRAW region; the FPGA lane-expands.
		volatile uint8_t *rom = win + ETH_NB_OFF_ROM;
		for (uint32_t i = 0; i < ETH_NB_ROM_SIZE; i++) rom[i] = 0;
		uint32_t off = ETH_NB_ROM_SIZE - declrom_len;
		for (uint32_t i = 0; i < declrom_len; i++) rom[off + i] = declrom[i];
		return;
	}
	// LC: flat image (byteLanes $0F) right-aligned in the top half of the 64K window.
	volatile uint8_t *rom = win + ETH_LC_OFF_ROM;
	for (uint32_t i = 0; i < 0x10000; i++) rom[i] = 0;
	uint32_t off = MAC_ETH_DECLROM_WINDOW_OFF + (MAC_ETH_DECLROM_SIZE - declrom_len);
	for (uint32_t i = 0; i < declrom_len; i++) rom[off + i] = declrom[i];
}

// PROM cooking (MAME enetlc.cpp/enetnbtp.cpp): 0-5 = bit-swizzled MAC, 6 = 0, 7 = XOR, complemented.
static uint8_t swizzle(uint8_t x)
{
	uint8_t lo = (uint8_t)(((x & 1) << 3) | ((x & 2) << 1) | ((x & 4) >> 1) | ((x & 8) >> 3));
	uint8_t hi = (uint8_t)((((x >> 4) & 1) << 3) | (((x >> 4) & 2) << 1) |
	                       (((x >> 4) & 4) >> 1) | (((x >> 4) & 8) >> 3));
	return (uint8_t)((lo << 4) | hi);
}

static void stage_macprom(void)
{
	uint8_t prom[8];
	uint8_t x = 0;
	for (int i = 0; i < 6; i++)
	{
		prom[i] = swizzle(guest_mac[i]);
		x ^= prom[i];
	}
	prom[6] = 0;
	prom[7] = (uint8_t)(x ^ 0xff);
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v |= (uint64_t)prom[i] << (8 * i);
	*ctl(ETH_CTL_MACPROM) = v;
}

static void push_state(void)
{
	uint16_t r[64];
	sonic_fill_shadows(r);
	for (int n = 0; n < 16; n++)
	{
		uint64_t v = 0;
		for (int k = 0; k < 4; k++)
			v |= (uint64_t)r[4 * n + k] << (16 * k);
		*ctl(ETH_CTL_SHAD + 8 * n) = v;
	}
	__sync_synchronize();   // the ISR the guest handler reads is never stale
	*ctl(ETH_CTL_INT) = sonic_int_line() ? 1 : 0;
}

// Doorbell ring: slurp (stash + publish RPTR) is safe inside a host op, apply re-enters the model.
#define STASH_DEPTH 1024
static uint64_t ring_stash[STASH_DEPTH];
static int stash_head, stash_count;

static void ring_slurp(void)
{
	uint32_t wp = (uint32_t)*ctl(ETH_CTL_WPTR);
	if (wp == rptr) return;
	if (wp < rptr)
	{
		printf("mac_eth: wptr regressed (%u < %u) — FPGA reset, resync\n", wp, rptr);
		rptr = 0;
	}
	uint32_t backlog = wp - rptr;
	if (backlog > st.ring_max) st.ring_max = backlog;
	if (backlog > ETH_RING_ENTRIES)
	{
		// Lapped: those slots hold newer entries now, so skip to what still exists.
		st.ring_ovf += backlog - ETH_RING_ENTRIES;
		rptr = wp - ETH_RING_ENTRIES;
	}
	int guard = ETH_RING_ENTRIES;
	while (rptr != wp && guard--)
	{
		uint64_t e = *ctl(ETH_CTL_RING + 8 * (rptr & (ETH_RING_ENTRIES - 1)));
		rptr++;
		if (stash_count < STASH_DEPTH)
			ring_stash[(stash_head + stash_count++) % STASH_DEPTH] = e;
		else
			st.ring_ovf++;   // stash full: a multi-second stall, writes lost
	}
	*ctl(ETH_CTL_RPTR) = rptr;   // release the guest-bus throttle
}

// Bounded + publish per applied entry: the guest ISR spins until its acks read back applied.
#define DRAIN_BUDGET 256
static void drain_ring(void)
{
	ring_slurp();
	int budget = DRAIN_BUDGET;
	while (stash_count && budget--)
	{
		uint64_t e = ring_stash[stash_head];
		stash_head = (stash_head + 1) % STASH_DEPTH;
		stash_count--;
		if (!(e & 1)) continue;
		int tag  = (int)(e >> 1) & 7;
		int r    = (int)(e >> 4) & 0x3f;
		int data = (int)(e >> 16) & 0xffff;
		switch (tag)
		{
		case ETH_TAG_REG_WR:
			st.reg_wr[r & 0x3f]++;
			if (r == SONIC_CR && (data & 0x0002)) st.txp_cmds++;   // CR_TXP
			sonic_reg_write(r, (uint16_t)data);
			break;
		case ETH_TAG_RESET:  sonic_reset(); break;
		}
		push_state();
	}
	if (stash_count) st.drain_full++;
}

// The OSD selection the running card was started with: a mid-session change restarts it in place.
#define SEL_NONE 0xFFFFFFFFu
static uint32_t sel_cur = SEL_NONE;
static uint32_t fail_announced = SEL_NONE;

static uint32_t sel_snapshot(void)
{
	return (user_io_status_get(ETH_OPT_IFACE) & 3) |
	       ((user_io_status_get(ETH_OPT_MACSUF) & 0xF) << 8);
}

static void announce_up(void)
{
	char msg[96];
	snprintf(msg, sizeof msg,
	         "Ethernet: %s\n%02X:%02X:%02X:%02X:%02X:%02X\n"
	         "restart the Mac to apply",
	         ifname, guest_mac[0], guest_mac[1], guest_mac[2],
	         guest_mac[3], guest_mac[4], guest_mac[5]);
	Info(msg, 3000);
}

// card_start() retries every second, so latch the message or it spams the OSD.
static void announce_down(const char *what)
{
	if (fail_announced == sel_cur) return;
	fail_announced = sel_cur;
	char msg[80];
	snprintf(msg, sizeof msg, "Ethernet: %s", what);
	Info(msg, 4000);
}

// Nothing is written until every failure path passes: no MAGIC means the slot stays open-bus.
static void card_start(void)
{
	card_kind = core_is_maciivi() ? CARD_NB : CARD_LC;
	ctrl_base = (card_kind == CARD_NB) ? ETH_NB_CTRL : ETH_LC_CTRL;
	load_config();
	sel_cur = sel_snapshot();

	// Keep retrying so a repaired ROM takes effect on the next core load; latch the complaint.
	int quiet = (fail_announced == sel_cur);
	if (!load_declrom(quiet))
	{
		announce_down("no declaration ROM");
		return;
	}
	if (!mac_eth_iface_open(ifname))
	{
		char msg[64];
		snprintf(msg, sizeof msg, "%s unavailable", ifname);
		announce_down(msg);
		printf("mac_eth: iface %s unavailable - card stays down\n", ifname);
		return;
	}
	fail_announced = SEL_NONE;

	sonic_init(card_kind == CARD_NB ? &nb_host_ops : &lc_host_ops);
	// NB SONIC addresses are slot forms masked into card RAM, never stripped to 24 bits.
	sonic_set_addr_bits(card_kind == CARD_NB ? 32 : addr_bits);

	rptr = (uint32_t)*ctl(ETH_CTL_WPTR);   // skip anything stale
	*ctl(ETH_CTL_RPTR) = rptr;             // release ring backpressure
	stash_head = stash_count = 0;          // stale stashed writes die with the session
	if (card_kind == CARD_LC)
	{
		dma_seq = 0;
		*ctl(ETH_CTL_DMACMD)  = 0;         // engine reset state: seq 0 done
		*ctl(ETH_CTL_DMASTAT) = 0;
	}

	stage_declrom();
	stage_macprom();
	push_state();
	*ctl(ETH_CTL_GEO) = (card_kind == CARD_NB) ? 3 : 2;   // layout version
	__sync_synchronize();
	*ctl(ETH_CTL_MAGIC) = (card_kind == CARD_NB) ? ETH_MAGIC_NB : ETH_MAGIC_LC;
	card_up = 1;
	printf("mac_eth: %s card up (iface %s, MAC %02X:%02X:%02X:%02X:%02X:%02X)\n",
	       card_kind == CARD_NB ? "NuBus" : "LC PDS", ifname,
	       guest_mac[0], guest_mac[1], guest_mac[2],
	       guest_mac[3], guest_mac[4], guest_mac[5]);
}

// Held-unicast redelivery: depth covers MacTCP's ~16 KB window, and 2 s outlives any ISR latency.
#define RXQ_DEPTH      64
#define RXQ_MAX_AGE_US 2000000ULL
static struct { uint8_t buf[1600]; int len; uint64_t t; } rxq[RXQ_DEPTH];
static int rxq_head, rxq_count;

static void rxq_reset(void) { rxq_head = rxq_count = 0; }

static void rxq_push(const uint8_t *f, int n)
{
	if (n > (int)sizeof rxq[0].buf || rxq_count == RXQ_DEPTH)
	{
		st.rx_refused++;   // tail-drop: a full queue means the guest stopped draining
		return;
	}
	int slot = (rxq_head + rxq_count) % RXQ_DEPTH;
	memcpy(rxq[slot].buf, f, n);
	rxq[slot].len = n;
	rxq[slot].t = now_us();
	rxq_count++;
	st.rx_held++;
	if ((uint64_t)rxq_count > st.rx_held_max) st.rx_held_max = rxq_count;
}

static void rxq_flush(void)
{
	uint64_t now = now_us();
	while (rxq_count)
	{
		if (now - rxq[rxq_head].t > RXQ_MAX_AGE_US)
		{
			st.rx_refused++;                    // expired: count as real loss
		} else
		{
			int r = sonic_rx_frame(rxq[rxq_head].buf, rxq[rxq_head].len);
			if (r < 0) return;                  // still busy: keep holding, in order
		}
		rxq_head = (rxq_head + 1) % RXQ_DEPTH;
		rxq_count--;
	}
}

static void card_stop(void)
{
	if (!card_up) return;
	*ctl(ETH_CTL_MAGIC) = 0;
	mac_eth_iface_close();
	card_up = 0;
	rxq_reset();   // held frames belong to the closed session
	printf("mac_eth: card down\n");
}

// Arm only for a Mac core with a card: MacLC uses the built-in ROM, others ship their own.
static int core_has_card(void)
{
	if (!is_mac_scsi_family()) return 0;
	if (core_is_maclc()) return 1;
	return FileExists(user_io_make_filepath(HomeDir(), DECLROM_NAME));
}

void mac_eth_poll(void)
{
	static unsigned long pace_timer, name_timer, stats_timer;
	static int mapped;

	// Presence latches at guest reset and cannot be un-latched: clear a previous session's MAGIC.
	static int purged;
	if (!purged)
	{
		purged = 1;
		volatile uint8_t *w = (volatile uint8_t *)shmem_map(ETH_DDR_BASE, ETH_WIN_SIZE);
		if (w)
		{
			// Both layouts: whichever core loads must find its gate clear.
			*(volatile uint64_t *)(w + ETH_LC_CTRL + ETH_CTL_MAGIC) = 0;
			*(volatile uint64_t *)(w + ETH_NB_CTRL + ETH_CTL_MAGIC) = 0;
			shmem_unmap((void *)w, ETH_WIN_SIZE);
		}
	}

	// ~1 ms service pace; core-name recheck each second
	if (!CheckTimer(pace_timer)) return;
	pace_timer = GetTimer(1);

	if (CheckTimer(name_timer))
	{
		name_timer = GetTimer(1000);
		int want = core_has_card();
		if (want && !card_up)
		{
			if (!mapped)
			{
				win = (volatile uint8_t *)shmem_map(ETH_DDR_BASE, ETH_WIN_SIZE);
				mapped = (win != NULL);
			}
			if (mapped) card_start();
		}
		else if (!want && card_up) card_stop();
		else if (want && card_up && sel_snapshot() != sel_cur)
		{
			// Presence latches at guest reset: a new MAC needs a guest restart.
			card_stop();
			card_start();
			if (card_up) announce_up();
		}
	}

	if (!card_up) return;

	drain_ring();
	// Alternate apply/resume until the chain ends: the guest spin-polls TXP with its tick frozen.
	sonic_tx_continue();
	for (int spins = 32; (sonic_reg(SONIC_CR) & SONIC_CR_TXP) && spins; spins--)
	{
		drain_ring();
		sonic_tx_continue();
	}

	// The driver's deadman: real elapsed time so ISR_TC fires when traffic stops.
	{
		static uint64_t tick_last;
		uint64_t nowu = now_us();
		if (tick_last && nowu > tick_last)
			sonic_time_tick((unsigned)(nowu - tick_last));
		tick_last = nowu;
	}

	// Elasticity: hold refused unicast in order so a burst tail costs ms, not a TCP timeout.
	rxq_flush();
	uint8_t frame[2048];
	uint64_t rx_t0 = now_us();
	while (now_us() - rx_t0 < 1000)
	{
		int n = mac_eth_iface_recv(frame, sizeof frame);
		if (n <= 0) break;
		st.rx_frames++; st.rx_bytes += n;
		// >1518 off the tap means an offload is coalescing; the model refuses those silently.
		if (n > 1518 && st.rx_jumbo++ == 0)
			printf("mac_eth: %d-byte frame off the tap - receive offload leaking (GRO?)\n", n);
		// The socket is promiscuous: count guest-addressed frames separately or the totals lie.
		int unicast_ours = !(frame[0] & 1) && !memcmp(frame, guest_mac, 6);
		if ((frame[0] & 1) || unicast_ours)
		{
			st.rx_ours++; st.rx_ours_bytes += n;
			if (n >= 14)
			{
				unsigned et = ((unsigned)frame[12] << 8) | frame[13];
				if (et == 0x0806) st.rx_arp++;
				else if (et == 0x0800) st.rx_ip++;
			}
		}
		// Order matters: while held frames exist a new unicast queues behind them.
		if (unicast_ours && rxq_count) { rxq_push(frame, n); continue; }
		if (sonic_rx_frame(frame, n) < 0)
		{
			// refused before any state was touched: hold unicast, drop the rest
			if (unicast_ours) rxq_push(frame, n);
			else st.rx_refused++;
		}
	}

	push_state();

	if (CheckTimer(stats_timer))
	{
		stats_timer = GetTimer(1000);
		st.drops += (uint64_t)mac_eth_iface_drops();
		FILE *f = fopen("/tmp/mac_eth_stats", "w");
		if (f)
		{
			fprintf(f, "rpc        %llu\n", (unsigned long long)st.rpc);
			fprintf(f, "rpc_slept  %llu\n", (unsigned long long)st.rpc_slept);
			fprintf(f, "rpc_us_avg %llu\n",
			        (unsigned long long)(st.rpc ? st.rpc_us / st.rpc : 0));
			fprintf(f, "rpc_us_max %llu\n", (unsigned long long)st.rpc_us_max);
			fprintf(f, "rx_frames  %llu\n", (unsigned long long)st.rx_frames);
			fprintf(f, "rx_bytes   %llu\n", (unsigned long long)st.rx_bytes);
			fprintf(f, "tx_frames  %llu\n", (unsigned long long)st.tx_frames);
			fprintf(f, "tx_bytes   %llu\n", (unsigned long long)st.tx_bytes);
			fprintf(f, "sock_drops %llu\n", (unsigned long long)st.drops);
			fprintf(f, "rx_refused %llu\n", (unsigned long long)st.rx_refused);
			fprintf(f, "rpc_fail   %llu\n", (unsigned long long)st.rpc_fail);
			fprintf(f, "txp_cmds   %llu\n", (unsigned long long)st.txp_cmds);
			fprintf(f, "tx_fail    %llu\n", (unsigned long long)st.tx_fail);
			fprintf(f, "ea_strip   %u\n", sonic_ea_stripped());
			fprintf(f, "redeliver  rx %u  tx %u\n",
			        sonic_redelivered_rx(), sonic_redelivered_tx());
			fprintf(f, "rx_held    %llu  max_depth %llu\n",
			        (unsigned long long)st.rx_held, (unsigned long long)st.rx_held_max);
			fprintf(f, "rx_jumbo   %llu\n", (unsigned long long)st.rx_jumbo);
			fprintf(f, "drain_full %llu\n", (unsigned long long)st.drain_full);
			fprintf(f, "rx_ours    %llu  bytes %llu\n",
			        (unsigned long long)st.rx_ours, (unsigned long long)st.rx_ours_bytes);
			fprintf(f, "arp        rx %llu  tx %llu\n",
			        (unsigned long long)st.rx_arp, (unsigned long long)st.tx_arp);
			fprintf(f, "ip         rx %llu  tx %llu\n",
			        (unsigned long long)st.rx_ip, (unsigned long long)st.tx_ip);
			fprintf(f, "ring_max   %u  ring_ovf %u\n", st.ring_max, st.ring_ovf);
			fprintf(f, "regwr");
			for (int i = 0; i < 64; i++)
				if (st.reg_wr[i]) fprintf(f, " %02X=%u", i, st.reg_wr[i]);
			fprintf(f, "\n");
			fprintf(f, "sonic_tx chains=%u pkts=%u eol=%u ab=%u ovs=%u laps=%u"
			        " busy=%u ke=%u ko=%u ksw=%u ctda=%04X\n",
			        sonic_txd.chains, sonic_txd.pkts, sonic_txd.ends_eol,
			        sonic_txd.aborts, sonic_txd.oversize, sonic_txd.laps,
			        sonic_txd.busy_stop, sonic_txd.kicks_even, sonic_txd.kicks_odd,
			        sonic_txd.kicks_swallowed, sonic_txd.last_ctda);
			fprintf(f, "sonic cr=%04X isr=%04X imr=%04X crda=%04X rrp=%04X rwp=%04X\n",
			        sonic_reg(SONIC_CR), sonic_reg(SONIC_ISR), sonic_reg(SONIC_IMR),
			        sonic_reg(SONIC_CRDA), sonic_reg(SONIC_RRP), sonic_reg(SONIC_RWP));
			fclose(f);
		}
	}
}
