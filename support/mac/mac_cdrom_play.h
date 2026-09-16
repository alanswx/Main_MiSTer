// Mac CD-ROM: the AppleCD playhead on the ARM. A transcription of the
// MacQuadra800 core's rtl/cd_audio.sv MAIN FSM (M_CMD / M_CTRK_RD /
// M_SCAN_GO / M_APPLY, the frame_done advance and the M_REF_* status
// refresh) plus the MODE SELECT page $0E mirror and the volume law from
// rtl/cd_vol_lut.vh. Oracles behind that RTL: MAME nscsi_cdrom_apple_device,
// Snow, BlueSCSI, and the 2026-07 AppleCD Audio Player captures.
//
// No Main dependencies: the Verilator block-device model compiles it too.
// Frames are handed out by mac_cd_play_frame() one per call, in the order
// the core consumes them; the playhead advances per frame delivered (the
// ide_cdda_send_sector model), so status leads the audio by the core's
// two-frame buffer, ~27 ms.

#ifndef MAC_CDROM_PLAY_H
#define MAC_CDROM_PLAY_H

#include <stdint.h>
#include "mac_cdrom_resp.h"

enum { MAC_CD_ST_IDLE = 0, MAC_CD_ST_PLAY = 1, MAC_CD_ST_PAUSE = 2, MAC_CD_ST_END = 3 };

struct mac_cd_play
{
	const mac_cd_toc *toc;      // NULL = no disc
	uint8_t  state;             // MAC_CD_ST_*
	uint32_t cur;               // playhead, 20-bit disc LBA (cd_audio.sv cur_lba)
	uint32_t stop;              // stop_lba
	uint32_t fetch;             // next frame to hand out (fetch_lba)
	uint8_t  scan;              // 0xCD scan in progress (+-8 frames per frame)
	uint8_t  scan_dir;          // 1 = rewind
	uint8_t  ports[4];          // page $0E: ch0, vol0, ch1, vol1
	uint32_t flush_gen;         // bumps on every reposition (the core drops buffered frames)
};

void     mac_cd_play_init(mac_cd_play *p);                          // power-on / machine reset
void     mac_cd_play_set_toc(mac_cd_play *p, const mac_cd_toc *t);  // (re)mount; NULL = unmount
void     mac_cd_play_stop(mac_cd_play *p);                          // data READ / eject / bus reset
// A CDB the core forwarded: the audio transport set, or MODE SELECT with its
// parameter list (list_len = cdb[4]; the list starts at list[0]).
void     mac_cd_play_command(mac_cd_play *p, const uint8_t *cdb, const uint8_t *list, int list_len);
// 1 = playing: *lba is the frame to deliver now and the playhead advanced.
int      mac_cd_play_frame(mac_cd_play *p, uint32_t *lba);
uint8_t  mac_cd_play_ast(const mac_cd_play *p);                     // 0 play, 1 paused, 3 end, 5 idle
void     mac_cd_play_pos(const mac_cd_play *p, mac_cd_pos *out);    // the M_REF_* refresh, on demand
uint16_t mac_cd_vol_gain(uint8_t v);                                // Q15, (v/255)^5
// Page $0E routing + volume on one frame of 16-bit LE stereo samples, in place.
void     mac_cd_play_scale(const mac_cd_play *p, int16_t *pcm, int stereo_samples);

#endif
