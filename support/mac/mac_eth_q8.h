// Quadra 800 SONIC: guest-RAM access through the core's op-list DMA engine (mac_eth.h layout v4).

#ifndef MAC_ETH_Q8_H
#define MAC_ETH_Q8_H

#include <stdint.h>

typedef struct
{
	volatile uint8_t  *xfer;   // ETH_Q8_XFER_SIZE staging bytes
	volatile uint64_t *ops;    // ETH_Q8_MAX_OPS op words
	volatile uint64_t *cmd;    // DMA_CMD
	volatile uint64_t *stat;   // DMA_STAT
	volatile uint64_t *isr_set;// ISR_SET
	volatile uint64_t *isr_ack;// ISR_ACK
	void (*idle)(void);        // run while the engine works (the doorbell ring must keep draining)
	int  (*wait)(uint8_t seq); // test hook: run an engine model instead of spinning on stat; 0 = done
} q8_mailbox;

void q8_init(const q8_mailbox *m);

// sonic_host_ops backend. Writes are deferred and ride in front of the next read, or q8_flush().
int  q8_read_words(uint32_t ga, uint16_t *w, int n, int stride);
int  q8_write_words(uint32_t ga, const uint16_t *w, int n, int stride);
int  q8_read_bytes(uint32_t ga, uint8_t *b, int n);
int  q8_write_bytes(uint32_t ga, const uint8_t *b, int n);

// Land every deferred write. Call before anything that tells the guest to look (shadows, ISR bits).
int  q8_flush(void);
// Forget the descriptor read-ahead: the guest may have rewritten RAM since.
void q8_begin(void);

// The FPGA owns ISR. The model's raises are posted as sequenced events, one post in flight at a
// time; each doorbell entry names the last post its guest write could have seen (`seen`).
void     q8_isr_reset(void);
void     q8_isr_post(void);                            // post what the model raised, if the last post landed
uint16_t q8_isr_qualify(uint16_t data, uint16_t seen); // the bits of an ISR ack the replica may clear

typedef struct
{
	uint64_t rpc, ops, rpc_us, rpc_us_max, rpc_slept, rpc_fail, bad_addr, ahead_hit;
	uint64_t isr_posts, isr_acks_kept;
	uint16_t isr_seq, isr_unposted;
} q8_stats_t;
extern q8_stats_t q8_stats;

#endif
