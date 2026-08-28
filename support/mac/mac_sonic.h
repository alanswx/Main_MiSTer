// DP83932/DP83934 SONIC model: registers here, descriptor and packet memory in guest RAM via ops.

#ifndef MAC_SONIC_H
#define MAC_SONIC_H

#include <stdint.h>

typedef struct
{
	// Grouped guest access: 16-bit BE words at even addresses; 0 on success, -1 on host failure.
	int (*read_words)(uint32_t gaddr, uint16_t *w, int n, int stride);
	int (*write_words)(uint32_t gaddr, const uint16_t *w, int n, int stride);
	int (*read_bytes)(uint32_t gaddr, uint8_t *b, int n);
	int (*write_bytes)(uint32_t gaddr, const uint8_t *b, int n);
	// Frame WITHOUT FCS: the transport appends its own (a software FCS breaks max-size sends).
	int (*wire_send)(const uint8_t *frame, int len);
} sonic_host_ops;

void     sonic_init(const sonic_host_ops *ops);
void     sonic_reset(void);                    // hardware/software reset state
void     sonic_reg_write(int reg, uint16_t data);
uint16_t sonic_reg(int reg);                   // current value (shadow source)
void     sonic_fill_shadows(uint16_t regs[64]);
int      sonic_int_line(void);                 // level: ISR & IMR & 0x7fff
// Deliver one frame (no FCS): 1 = delivered, 0 = dropped, -1 = busy before any state changed.
int      sonic_rx_frame(const uint8_t *frame, int len);
// Resume a budget-suspended transmit chain from the poll; a no-op when none is suspended.
void     sonic_tx_continue(void);
// Advance the watchdog by `us` of wall time: the driver's deadman, acked through ISR_TC.
void     sonic_time_tick(unsigned us);
uint32_t sonic_ea_stripped(void);              // dirty-top-byte addrs masked (24-bit-mode witness)
uint32_t sonic_redelivered_rx(void);           // PKTRX acks re-asserted by the redelivery guard
uint32_t sonic_redelivered_tx(void);           // TXDN acks re-asserted by the redelivery guard
void     sonic_set_addr_bits(int bits);        // slot address lines: 24 = PDS, 32 = NuBus
// TX-path witnesses (counters only; see the block above transmit_chain)
typedef struct
{
	uint32_t chains, pkts, ends_eol, aborts, oversize, laps, busy_stop;
	uint32_t kicks_even, kicks_odd, kicks_swallowed;
	uint16_t last_ctda;
} sonic_tx_debug_t;
extern sonic_tx_debug_t sonic_txd;

#endif
