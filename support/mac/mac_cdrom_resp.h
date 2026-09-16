// Mac CD-ROM: the AppleCD (SONY CDU-8004) response builders, pure functions
// of the disc TOC, the playhead position and the CDB fields the response
// depends on. Byte-exact to the MacQuadra800 core's rtl/ncr53c96.sv +
// rtl/cd_audio.sv of 2026-09-16 (themselves MAME nscsi_cdrom_apple_device
// oracles), including that RTL's table caps: 60 tracks in READ TOC format 0,
// 41 in format 2, 99 in the Apple $C1 TOC (last descriptor repeated).
//
// No Main dependencies on purpose: the core's Verilator block-device model
// compiles this same file, so the sim and the box serve identical bytes.
// Served through the CD slot's response window (mac_cdrom.h) by cores that
// pass is_mac_scsi_optimized(); older cores build these in RTL from the
// MCDA blob and never read the window.

#ifndef MAC_CDROM_RESP_H
#define MAC_CDROM_RESP_H

#include <stdint.h>

#define MAC_CD_MAX_TRACKS 99

// Disc LBAs are 0-based (INDEX 01, no +150); the builders add the 150-frame
// pregap where the MMC responses want it. n is 1..99.
struct mac_cd_toc
{
	int      n;
	uint8_t  ctrl[MAC_CD_MAX_TRACKS];     // ADR/control: 0x10 audio, 0x14 data
	uint32_t start[MAC_CD_MAX_TRACKS];
	uint32_t pregap[MAC_CD_MAX_TRACKS];
	uint32_t leadout;
	int      data_trk;                    // 0-based served data track, -1 = audio-only disc
};

// The playhead as cd_audio.sv's status refresh publishes it.
struct mac_cd_pos
{
	uint8_t ast;                          // Apple audio status: 0 play, 1 paused, 3 end, 5 idle
	uint8_t ctrl;                         // current track's ADR/control
	uint8_t trk;                          // BINARY, 1-based
	uint8_t abs_m, abs_s, abs_f;          // BINARY, no +150
	uint8_t rel_m, rel_s, rel_f;
};

// RTL arithmetic, kept bit-for-bit (the 8-bit BCD helpers and the 20-bit
// M/S/F divider with its 99-minute clamp and 7-bit seconds counter).
uint8_t mac_cd_bin2bcd(uint8_t v);
uint8_t mac_cd_bcd2bin(uint8_t b);
void    mac_cd_lba2msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f);

// Every builder zeroes a 512-byte block, writes the response at its start
// and returns the natural length the core serves for an unlimited
// allocation (the core clamps to the CDB's allocation length itself).
int mac_cd_resp_inquiry(uint8_t *out);                                   // 54
int mac_cd_resp_mode_sense(uint8_t page, uint32_t last_lba,
                           const uint8_t ports[4], uint8_t *out);        // 36 / 28 / 38 / 12
int mac_cd_resp_toc_c1(const mac_cd_toc *t, uint8_t cdb9, uint8_t cdb5, uint8_t *out); // 4 or 400
int mac_cd_resp_toc_43(const mac_cd_toc *t, uint8_t cdb9, uint8_t cdb6, uint8_t *out); // 512
int mac_cd_resp_subch(const mac_cd_pos *p, uint8_t cdb3, uint8_t cdb6, uint8_t *out);  // 64
int mac_cd_resp_subq(const mac_cd_pos *p, uint8_t *out);                 // 9
int mac_cd_resp_astat(const mac_cd_pos *p, uint8_t cdb3, uint8_t *out);  // 6

// The RTL's three pre-rendered tables, for the golden-bytes test against a
// table dump of cd_audio.sv: $C1 (404 bytes), $43 format 0 (returns the
// table length, first/last + descriptors + lead-out row) and format 2 with
// the format-1 session page at [496..507] (returns the format-2 length).
void mac_cd_table_c1(const mac_cd_toc *t, uint8_t *tab404);
int  mac_cd_table_43(const mac_cd_toc *t, uint8_t *tab512);
int  mac_cd_table_2(const mac_cd_toc *t, uint8_t *tab512);

// 1 = the disc has a data track, as the RTL judges it (control bit 2 over
// the first 41 tracks); data READs on a disc without one CHECK with $64.
int mac_cd_has_data(const mac_cd_toc *t);

// The MCDA blob (two 512-byte blocks at MAC_CDROM_TOC_BLK), version 2:
//   [0..3] "MCDA" [4] version=2 [5] first=1 [6] last [7] data track 1-based
//   [8..11] lead-out LE  [12] flags: bit0 has data track, bit1 has audio
//   [16+8k] track k: ctrl, 0, start LE32, pregap LE16 (clamped)
// Version-1 readers use bytes 0..11 and the entries only.
void mac_cd_build_blob(const mac_cd_toc *t, uint8_t *out1024);

#endif
