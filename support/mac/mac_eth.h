// Mac Ethernet cards: DDR3 mailbox layouts, mirrored by the cores' RTL (LC v2, NuBus v3).

#ifndef MAC_ETH_H
#define MAC_ETH_H

#include <stdint.h>

// DDR3 window (ARM physical); the map covers the larger (v3) layout.
#define ETH_DDR_BASE   0x1FF00000UL
#define ETH_WIN_SIZE   0x29000UL

// LC card (Apple Ethernet LC Twisted Pair, 820-0532-B): layout v2.
#define ETH_LC_OFF_XFER 0x00000UL  // 64K guest-RAM DMA bounce buffer
#define ETH_LC_OFF_ROM  0x10000UL  // 64K declROM window: byte i = guest $FEFF0000+i
#define ETH_LC_CTRL     0x20000UL  // control block base
#define ETH_LC_WIN_SIZE 0x21000UL
#define ETH_MAGIC_LC    0x4D634C4345544832ULL   // "McLCETH2"

// NuBus card (Apple Ethernet NB Twisted Pair, 820-0511-A): layout v3.
#define ETH_NB_OFF_RAM  0x00000UL  // 128K on-card RAM: byte i = card byte i
#define ETH_NB_RAM_SIZE 0x20000UL
#define ETH_NB_OFF_ROM  0x20000UL  // 32K RAW declROM (the FPGA lane-expands byteLanes $D2)
#define ETH_NB_ROM_SIZE 0x8000UL
#define ETH_NB_CTRL     0x28000UL  // control block base
#define ETH_NB_WIN_SIZE 0x29000UL
#define ETH_MAGIC_NB    0x4D634E4245544833ULL   // "McNBETH3"

// Control block (identical internal layout in both windows), relative to its base:
#define ETH_CTL_MAGIC   0x000UL    // ARM->FPGA presence gate, written LAST
#define ETH_CTL_WPTR    0x008UL    // FPGA->ARM doorbell write index (monotonic)
#define ETH_CTL_SHAD    0x010UL    // 16 words: regs 4n..4n+3, reg 4n+k at bits [16k+15:16k]
#define ETH_CTL_INT     0x090UL    // bit0 = SONIC INT line
#define ETH_CTL_MACPROM 0x098UL    // 8 cooked PROM bytes (byte k = PROM byte k)
#define ETH_CTL_GEO     0x0A0UL    // layout version (2 = LC, 3 = NB)
#define ETH_CTL_RPTR    0x0A8UL    // ARM ring read index (doorbell backpressure)
#define ETH_CTL_DMACMD  0x0B0UL    // LC only: [7:0] seq | [8] dir | [39:16] addr | [55:40] count
#define ETH_CTL_DMASTAT 0x0B8UL    // LC only: [7:0] seq echo | [8] error
#define ETH_CTL_RING    0x800UL    // 256 u64: valid|tag[3:1]|reg[9:4]|data[31:16]|seq[39:32]
#define ETH_RING_ENTRIES 256

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

#endif
