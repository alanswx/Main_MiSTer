// Mac Ethernet: DDR3 mailbox layouts, mirrored by the cores' RTL (LC card v2, Quadra 800 onboard v4).

#ifndef MAC_ETH_H
#define MAC_ETH_H

#include <stdint.h>

// DDR3 window (ARM physical); the map covers the larger (LC) layout.
#define ETH_DDR_BASE   0x1FF00000UL
#define ETH_WIN_SIZE   0x21000UL

// LC card (Apple Ethernet LC Twisted Pair, 820-0532-B): layout v2.
#define ETH_LC_OFF_XFER 0x00000UL  // 64K guest-RAM DMA bounce buffer
#define ETH_LC_OFF_ROM  0x10000UL  // 64K declROM window: byte i = guest $FEFF0000+i
#define ETH_LC_CTRL     0x20000UL  // control block base
#define ETH_LC_WIN_SIZE 0x21000UL
#define ETH_MAGIC_LC    0x4D634C4345544832ULL   // "McLCETH2"

// Control block, relative to its base (LC layout):
#define ETH_CTL_MAGIC   0x000UL    // ARM->FPGA presence gate, written LAST
#define ETH_CTL_WPTR    0x008UL    // FPGA->ARM doorbell write index (monotonic)
#define ETH_CTL_SHAD    0x010UL    // 16 words: regs 4n..4n+3, reg 4n+k at bits [16k+15:16k]
#define ETH_CTL_INT     0x090UL    // bit0 = SONIC INT line
#define ETH_CTL_MACPROM 0x098UL    // 8 cooked PROM bytes (byte k = PROM byte k)
#define ETH_CTL_GEO     0x0A0UL    // layout version (2 = LC, 4 = Quadra 800)
#define ETH_CTL_RPTR    0x0A8UL    // ARM ring read index (doorbell backpressure)
#define ETH_CTL_DMACMD  0x0B0UL    // [7:0] seq | [8] dir | [39:16] addr | [55:40] count
#define ETH_CTL_DMASTAT 0x0B8UL    // [7:0] seq echo | [8] error
#define ETH_CTL_RING    0x800UL    // 256 u64: valid|tag[3:1]|reg[9:4]|data[31:16]|seq[47:32]
#define ETH_RING_ENTRIES 256

// Quadra 800 onboard SONIC: layout v4. MAGIC/WPTR/SHAD/RING sit where the LC has them.
#define ETH_Q8_OFF_XFER 0x0000UL   // 16K DMA staging: each op's bytes start 8-aligned
#define ETH_Q8_XFER_SIZE 0x4000UL
#define ETH_Q8_CTRL     0x4000UL   // control block base
#define ETH_Q8_WIN_SIZE 0x5000UL
#define ETH_MAGIC_Q8    0x4D63513845544834ULL   // "McQ8ETH4"
#define ETH_Q8_ISRSET   0x090UL    // ARM->FPGA [15:0] seq | [30:16] bits to OR into the FPGA's ISR
#define ETH_Q8_ISRACK   0x098UL    // FPGA->ARM [15:0] seq consumed
#define ETH_Q8_MACPROM  0x0A0UL    // 8 cooked PROM bytes, read on demand
#define ETH_Q8_GEO      0x0A8UL
#define ETH_Q8_PTRS     0x0B0UL    // [31:0] ring read index | [63:32] applied-and-pushed index
#define ETH_Q8_DMACMD   0x0B8UL    // [7:0] seq | [11:8] op count
#define ETH_Q8_DMASTAT  0x0C0UL    // FPGA->ARM [7:0] seq echo
#define ETH_Q8_DEBUG    0x0C8UL    // FPGA->ARM [14:0] ISR | [29:15] IMR | [30] present | [31] irq, as the guest sees them
#define ETH_Q8_SAMPLE   0x0D0UL    // FPGA->ARM [31:0] CPU PC | [47:32] SR, once per FPGA poll round
#define ETH_Q8_OPS      0x100UL    // 8 u64: [0] dir (1 = to guest) | [31:16] bytes | [63:32] guest addr
#define ETH_Q8_MAX_OPS  8
#define ETH_Q8_RAM_TOP  0x08000000UL   // the engine carries addr[26:2]: 128 MB of RAM, nothing else

#define ETH_TAG_REG_WR 0
#define ETH_TAG_RESET  1

// Public API (called from mac.cpp).
void mac_eth_poll(void);   // self-gating: Mac-core detection, lazy arm/teardown, bounded per pass

// Network iface layer (mac_eth_iface.cpp).
int  mac_eth_iface_open(const char *name);
void mac_eth_iface_close(void);
int  mac_eth_iface_send(const uint8_t *frame, int len);
int  mac_eth_iface_recv(uint8_t *buf, int maxlen);
int  mac_eth_iface_fd(void);
int  mac_eth_iface_drops(void);
int  mac_eth_iface_hwaddr(const char *name, uint8_t mac[6]);   // 1 = the interface has an address

#endif
