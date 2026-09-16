// Mac CD-ROM slot: serves CUE/CHD/raw-sector images as a flat disc of
// 2048-byte sectors; flat 2048 images pass through the generic sd path.

#ifndef MAC_CDROM_H
#define MAC_CDROM_H

#include <stdint.h>

#define MAC_CDROM_SLOT 4   // MacLC.sv VD_CDROM (per-core routing: mac_cdrom_slot())

// Out-of-band windows above the data region, addressed directly by the RTL and
// invisible to the guest. Disc LBAs are 0-based; the RTL adds the +150 for MSF.
#define MAC_CDROM_TOC_BLK    0x7FFF0000u   // "MCDA" TOC blob (mac_cd_build_blob)
#define MAC_CDROM_TOC_BLKS   2u            // TOC blob length, 512-byte blocks
#define MAC_CDROM_AUDIO_BLK  0x40000000u   // CD-DA frames: lba = base + disc_lba

// Windows of the optimized cores (is_mac_scsi_optimized), served mounted or
// not. The core forwards a CD command's DATA IN through the response window
// and its CDB (+ DATA OUT list at byte 16) through the command block;
// audio comes one frame per next-frame read, the playhead state in the pad.
//   response:   read  1 block  at RESP_BLK + (op << 16) + (a << 8) + b
//               op $12 INQUIRY; $1A a = page; $43 a = cdb[9], b = cdb[6];
//               $C1 a = cdb[9], b = cdb[5]; $42 a = cdb[3], b = cdb[6];
//               $C2; $CC a = cdb[3]
//   command:    write 1 block  at CMD_BLK + (op << 16): bytes 0..11 the CDB,
//               16.. the parameter list; pseudo-ops $FF machine reset,
//               $FE SCSI bus reset
//   next frame: read  5 blocks at FRAME_BLK: 2352 bytes of volume-scaled PCM
//               (16-bit LE stereo), then [2352] audio status (0 play, 1
//               paused, 3 end, 5 idle), [2353] 1 = a frame is present,
//               [2354..2357] flush generation LE (bumps on every
//               reposition; the core drops frames fetched before it)
#define MAC_CDROM_FRAME_BLK  0x7C000000u
#define MAC_CDROM_CMD_BLK    0x7D000000u
#define MAC_CDROM_RESP_BLK   0x7E000000u
#define MAC_CDROM_WIN_BASE   MAC_CDROM_FRAME_BLK   // lowest window LBA; the CD-DA window ends here

enum
{
	MAC_CDROM_PASSTHRU = 0, // flat 2048-byte image: use the generic path
	MAC_CDROM_HANDLED  = 1, // translation active: serve via mac_cdrom_fill
	MAC_CDROM_REJECT   = 2, // unusable image (bad cue/chd): fail the mount
};

int mac_cdrom_mount(int index, const char *name);
uint64_t mac_cdrom_size(int index);

// True when translation is live on this index (mac_cdrom_fill serves it).
int mac_cdrom_active(int index);

// sz=512: data/TOC blocks (lba in 512-byte units); sz=2352: whole CD-DA frames.
void mac_cdrom_fill(int index, uint64_t lba, uint8_t *buf, int sz);
void mac_cdrom_unmount(int index);

// One-shot boot repulse (from user_io_poll): re-fires the mount ~60 s after
// attach unless the guest read the disc (AppleCD misses the early attach pulse).
void mac_cdrom_poll(void);

// Optimized-core windows (see above), routed by mac_sd_service whether or
// not a disc is mounted: a response / next-frame read (buf zeroed and
// filled, sz = 512 or 2560) and a command-block write.
void mac_cdrom_window_fill(uint32_t lba, uint8_t *buf, int sz);
void mac_cdrom_command(uint32_t lba, const uint8_t *buf, int sz);

#endif
