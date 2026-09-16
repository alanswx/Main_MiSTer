// Host-side test of the Mac CD-ROM response builders and playhead
// (support/mac/mac_cdrom_resp.cpp, mac_cdrom_play.cpp). Not part of the
// Main build (the Makefile globs one level of support/). Driven by the
// MacQuadra800 core's scripts/cd_resp_golden.sh:
//
//   cdrom_resp_test gen <dir>                  write <name>.blob.hex per TOC shape
//   cdrom_resp_test check <dir> <cd_vol_lut.vh>  compare against <name>.rtl.bin
//                                              dumped by verilator/tb_cd_audio_dump,
//                                              then the unit tests
//
// A .rtl.bin is 404 + 512 + 512 bytes ($C1, $43 format 0, format 2 tables
// as the RTL planes hold them) followed by "<disc_audio> <toc43_len>
// <toc2_len>" in a .rtl.txt next to it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include "../mac_cdrom_resp.h"
#include "../mac_cdrom_play.h"

static int fails = 0, checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct shape { const char *name; mac_cd_toc toc; };

static void audio(mac_cd_toc *t, int k, uint32_t start) { t->ctrl[k] = 0x10; t->start[k] = start; t->pregap[k] = 150; }
static void data(mac_cd_toc *t, int k, uint32_t start)  { t->ctrl[k] = 0x14; t->start[k] = start; t->pregap[k] = 150; }

static std::vector<shape> shapes()
{
	std::vector<shape> v;
	{
		shape s = { "iso", {} };
		s.toc.n = 1; data(&s.toc, 0, 0); s.toc.leadout = 123456; s.toc.data_trk = 0;
		v.push_back(s);
	}
	{
		// mixed mode: one data track then audio, starts exercising every BCD digit
		shape s = { "mixed", {} };
		s.toc.n = 7;
		data(&s.toc, 0, 0);
		audio(&s.toc, 1, 17359);    // 03:51:34
		audio(&s.toc, 2, 45000);    // 10:00:00
		audio(&s.toc, 3, 101234);   // 22:29:59
		audio(&s.toc, 4, 199999);
		audio(&s.toc, 5, 250075);
		audio(&s.toc, 6, 299850);
		s.toc.leadout = 300000; s.toc.data_trk = 0;
		v.push_back(s);
	}
	{
		// audio-only, 99 tracks to the 20-bit edge
		shape s = { "t99", {} };
		s.toc.n = 99;
		for (int k = 0; k < 99; k++) audio(&s.toc, k, 150 + k * 4533);
		s.toc.leadout = 449999; s.toc.data_trk = -1;
		v.push_back(s);
	}
	{
		// 70 tracks: past the format-0 (60) and format-2 (41) caps, data track last
		shape s = { "t70", {} };
		s.toc.n = 70;
		for (int k = 0; k < 69; k++) audio(&s.toc, k, k * 3001);
		data(&s.toc, 69, 69 * 3001);
		s.toc.leadout = 69 * 3001 + 20000; s.toc.data_trk = 69;
		v.push_back(s);
	}
	return v;
}

static int gen(const char *dir)
{
	for (const shape &s : shapes())
	{
		uint8_t blob[1024];
		mac_cd_build_blob(&s.toc, blob);
		std::string fn = std::string(dir) + "/" + s.name + ".blob.hex";
		FILE *f = fopen(fn.c_str(), "w");
		if (!f) { printf("cannot write %s\n", fn.c_str()); return 1; }
		for (int i = 0; i < 1024; i++) fprintf(f, "%02x%c", blob[i], (i % 16 == 15) ? '\n' : ' ');
		fclose(f);
		printf("wrote %s\n", fn.c_str());
	}
	return 0;
}

static void diff(const char *what, const uint8_t *got, const uint8_t *want, int n)
{
	for (int i = 0; i < n; i++)
	{
		checks++;
		if (got[i] != want[i])
		{
			fails++;
			printf("FAIL %s byte %d: builder %02x, RTL %02x\n", what, i, got[i], want[i]);
		}
	}
}

static int check_rtl(const char *dir)
{
	for (const shape &s : shapes())
	{
		std::string fn = std::string(dir) + "/" + s.name + ".rtl.bin";
		FILE *f = fopen(fn.c_str(), "rb");
		if (!f) { printf("SKIP %s: no RTL dump\n", fn.c_str()); continue; }
		uint8_t rtl[404 + 512 + 512];
		size_t got = fread(rtl, 1, sizeof(rtl), f);
		fclose(f);
		CHECK(got == sizeof(rtl), "%s: dump is %zu bytes", fn.c_str(), got);
		int rtl_audio = -1, rtl_l43 = -1, rtl_l2 = -1;
		std::string tn = std::string(dir) + "/" + s.name + ".rtl.txt";
		FILE *t = fopen(tn.c_str(), "r");
		if (t) { if (fscanf(t, "%d %d %d", &rtl_audio, &rtl_l43, &rtl_l2) != 3) rtl_audio = -1; fclose(t); }

		uint8_t c1[404], t43[512], t2[512];
		mac_cd_table_c1(&s.toc, c1);
		int l43 = mac_cd_table_43(&s.toc, t43);
		int l2  = mac_cd_table_2(&s.toc, t2);
		std::string w;
		w = s.name + std::string(" $C1");  diff(w.c_str(), c1, rtl, 404);
		w = s.name + std::string(" $43");  diff(w.c_str(), t43, rtl + 404, 512);
		w = s.name + std::string(" fmt2"); diff(w.c_str(), t2, rtl + 916, 512);
		CHECK(rtl_l43 < 0 || rtl_l43 == l43, "%s toc43_len: builder %d, RTL %d", s.name, l43, rtl_l43);
		CHECK(rtl_l2 < 0 || rtl_l2 == l2, "%s toc2_len: builder %d, RTL %d", s.name, l2, rtl_l2);
		CHECK(rtl_audio < 0 || rtl_audio == !mac_cd_has_data(&s.toc), "%s disc_audio: builder %d, RTL %d",
		      s.name, !mac_cd_has_data(&s.toc), rtl_audio);
		printf("%-6s compared against the RTL dump\n", s.name);
	}
	return 0;
}

static void check_vol_lut(const char *vh)
{
	FILE *f = fopen(vh, "r");
	if (!f) { printf("SKIP volume law: cannot open %s\n", vh); return; }
	char line[256];
	int n = 0;
	while (fgets(line, sizeof(line), f))
	{
		int v, g;
		if (sscanf(line, " 8'd%d: cd_vol_gain = 16'd%d;", &v, &g) == 2)
		{
			CHECK(mac_cd_vol_gain((uint8_t)v) == g, "vol %d: builder %d, LUT %d", v, mac_cd_vol_gain((uint8_t)v), g);
			n++;
		}
	}
	fclose(f);
	CHECK(n == 256, "volume LUT: %d entries parsed", n);
}

static void unit_tests()
{
	uint8_t out[512];

	// BCD helpers: the RTL's (v*205)>>11 quotient is v/10 for every byte
	for (int v = 0; v < 256; v++)
		CHECK(mac_cd_bin2bcd((uint8_t)v) == ((((v / 10) & 15) << 4) | (v % 10)), "bin2bcd %d", v);
	CHECK(mac_cd_bcd2bin(0x99) == 99 && mac_cd_bcd2bin(0x07) == 7, "bcd2bin");

	// the divider: 99-minute clamp, 7-bit seconds
	uint8_t m, s, fr;
	mac_cd_lba2msf(17359, &m, &s, &fr);  CHECK(m == 3 && s == 51 && fr == 34, "msf 17359 -> %d:%d:%d", m, s, fr);
	mac_cd_lba2msf(449999, &m, &s, &fr); CHECK(m == 99 && s == 59 && fr == 74, "msf 449999 -> %d:%d:%d", m, s, fr);
	mac_cd_lba2msf(0xFFFFF, &m, &s, &fr); CHECK(m == 99, "msf sentinel m=%d", m);

	// INQUIRY identity
	CHECK(mac_cd_resp_inquiry(out) == 54, "inquiry len");
	CHECK(out[0] == 0x05 && out[1] == 0x80 && !memcmp(out + 8, "SONY    CD-ROM CDU-8004 1.9a", 28), "inquiry text");
	CHECK(out[39] == 0xd0 && out[50] == 0xfe && out[51] == 0 && out[54] == 0, "inquiry tail");

	// MODE SENSE pages and header
	uint8_t ports[4] = { 1, 200, 2, 100 };
	CHECK(mac_cd_resp_mode_sense(0x30, 0x123456, ports, out) == 36, "ms30 len");
	CHECK(out[0] == 35 && out[2] == 0x80 && out[3] == 8 && out[5] == 0x12 && out[6] == 0x34 && out[7] == 0x56 && out[10] == 8, "ms30 header");
	CHECK(out[12] == 0x30 && out[13] == 0 && !memcmp(out + 14, "APPLE COMPUTER, INC   ", 22) && out[36] == 0, "ms30 page");
	CHECK(mac_cd_resp_mode_sense(0x0E, 0, ports, out) == 28 && out[0] == 27 && out[12] == 0x0E && out[13] == 0x0E &&
	      out[14] == 4 && out[18] == 75 && out[19] == 75 && out[20] == 1 && out[21] == 200 && out[22] == 2 && out[23] == 100, "ms0E");
	CHECK(mac_cd_resp_mode_sense(0x2A, 0, ports, out) == 38 && out[12] == 0x2A && out[13] == 0x18 && out[16] == 0x71 &&
	      out[18] == 0x28 && out[19] == 3 && out[22] == 1, "ms2A");
	CHECK(mac_cd_resp_mode_sense(0x01, 0xFFFFFFFF, ports, out) == 12 && out[0] == 11 && out[5] == 0xFF && out[7] == 0xFF && out[12] == 0, "ms other");

	std::vector<shape> sh = shapes();
	const mac_cd_toc *mixed = &sh[1].toc, *t70 = &sh[3].toc;

	// $43 format 0: start-track filter and the rewritten length
	mac_cd_resp_toc_43(mixed, 0x00, 0x00, out);
	CHECK(out[0] == 0 && out[1] == 8 * 8 + 2 && out[2] == 1 && out[3] == 7 && out[6] == 1 && out[4 + 8 * 7 + 2] == 0xAA, "t43 all");
	CHECK(out[4 + 8 * 8] == 0 && out[4 + 8 * 8 + 5] == 0, "t43 zero past the payload");
	mac_cd_resp_toc_43(mixed, 0x00, 0xAA, out);
	CHECK(out[0] == 0 && out[1] == 10 && out[2] == 1 && out[3] == 7 && out[6] == 0xAA && out[9] == 66 && out[12] == 0, "t43 lead-out only");
	mac_cd_resp_toc_43(mixed, 0x00, 0x06, out);
	CHECK(out[1] == 3 * 8 + 2 && out[6] == 6 && out[14] == 7 && out[22] == 0xAA, "t43 from track 6");
	mac_cd_resp_toc_43(mixed, 0x00, 0x09, out);
	CHECK(out[1] == 10 && out[6] == 0xAA, "t43 start past the last track = lead-out only");
	mac_cd_resp_toc_43(t70, 0x00, 0x00, out);
	CHECK(out[0] == 1 && out[1] == 0xEA && out[3] == 70 && out[4 + 8 * 60 + 2] == 0xAA && out[4 + 8 * 60 + 1] == 0x10, "t43 60-track cap, ctrl of the last row");

	// format 1 session page, format 2 A-rows and the 41 cap
	mac_cd_resp_toc_43(mixed, 0x40, 0x00, out);
	CHECK(out[1] == 0x0A && out[2] == 1 && out[5] == 0x14 && out[6] == 1 && out[9] == 0 && out[10] == 2 && out[11] == 0 && out[12] == 0, "t43 fmt1");
	mac_cd_resp_toc_43(t70, 0x80, 0x00, out);
	CHECK(out[0] == ((44 * 11 + 2) >> 8) && out[1] == ((44 * 11 + 2) & 0xFF) && out[7] == 0xA0 && out[18] == 0xA1 && out[23] == 0x41 &&
	      out[29] == 0xA2 && out[37 + 11 * 40 + 3] == 41 && out[37 + 11 * 41] == 0, "t43 fmt2 rows and cap");

	// $C1: header, lead-out, descriptors from a BCD track, clamp past 99
	mac_cd_resp_toc_c1(mixed, 0x00, 0x00, out);
	CHECK(out[0] == 1 && out[1] == 0x07 && out[2] == 0 && out[4] == 0, "c1 header");
	mac_cd_resp_toc_c1(mixed, 0x40, 0x00, out);
	CHECK(out[0] == 0x66 && out[1] == 0x40 && out[2] == 0x00 && out[3] == 0, "c1 lead-out 66:40:00 got %02x %02x %02x", out[0], out[1], out[2]);
	mac_cd_resp_toc_c1(mixed, 0x80, 0x02, out);
	CHECK(out[0] == 0x10 && out[1] == 0x03 && out[2] == 0x51 && out[3] == 0x34, "c1 track 2 descriptor");
	CHECK(out[4 * 5] == 0x10 && out[4 * 5 + 1] == 0x66 && out[4 * 6] == 0x10 && out[4 * 6 + 1] == 0x66, "c1 repeats the last track");
	CHECK(out[396] == 0x10 && out[399] == out[4 * 6 + 3], "c1 400-byte form clamps to descriptor 98");
	mac_cd_resp_toc_c1(mixed, 0x80, 0x99, out);
	CHECK(out[0] == 0x10 && out[1] == 0x66, "c1 track 99 = descriptor 98");

	// position responses
	mac_cd_pos pos = { 0, 0x10, 5, 4, 26, 45, 0, 40, 3 };
	mac_cd_resp_subch(&pos, 1, 0, out);
	CHECK(out[1] == 0x11 && out[3] == 12 && out[4] == 1 && out[5] == 0x10 && out[6] == 5 && out[7] == 1 &&
	      out[9] == 4 && out[10] == 26 && out[11] == 45 && out[13] == 0 && out[14] == 40 && out[15] == 3, "subch fmt1");
	mac_cd_resp_subch(&pos, 3, 0x21, out);
	CHECK(out[1] == 0x11 && out[3] == 20 && out[4] == 3 && out[6] == 0x21 && out[5] == 0, "subch fmt3");
	mac_cd_resp_subq(&pos, out);
	CHECK(out[0] == 0x10 && out[1] == 0x05 && out[2] == 1 && out[3] == 0x00 && out[4] == 0x40 && out[5] == 0x03 &&
	      out[6] == 0x04 && out[7] == 0x26 && out[8] == 0x45, "subq");
	pos.ast = 1;
	mac_cd_resp_astat(&pos, 0, out);
	CHECK(out[0] == 1 && out[1] == 0 && out[2] == 0x10 && out[3] == 0x04 && out[4] == 0x26 && out[5] == 0x45, "astat");
	mac_cd_resp_astat(&pos, 1, out);
	CHECK(out[0] == 0xFF && out[1] == 0xFF && out[2] == 0x10, "astat volumes form");

	// ---- playhead ----
	mac_cd_play p;
	mac_cd_play_init(&p);
	CHECK(p.ports[0] == 1 && p.ports[1] == 0xFF && p.ports[2] == 2 && p.ports[3] == 0xFF && p.state == MAC_CD_ST_IDLE, "play init");
	mac_cd_play_set_toc(&p, mixed);
	uint8_t cdb[12];
	uint32_t lba;

	// vendor PLAY track 3 (BCD): start(2) .. start(3)
	memset(cdb, 0, 12); cdb[0] = 0xC9; cdb[5] = 0x03; cdb[9] = 0x80;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY && p.cur == 45000 && p.stop == 300000 && p.fetch == 45000, "C9 track form plays to the lead-out: st %d cur %u stop %u", p.state, p.cur, p.stop);
	CHECK(mac_cd_play_frame(&p, &lba) == 1 && lba == 45000 && p.cur == 45001, "first frame");
	mac_cd_play_pos(&p, &pos);
	CHECK(pos.trk == 3 && pos.ctrl == 0x10 && pos.abs_m == 10 && pos.abs_s == 0 && pos.abs_f == 1 && pos.rel_f == 1, "pos in track 3: trk %d abs %d:%d:%d", pos.trk, pos.abs_m, pos.abs_s, pos.abs_f);

	// standard PLAY AUDIO TRACK/INDEX 2..2 then run to the end
	memset(cdb, 0, 12); cdb[0] = 0x48; cdb[4] = 2; cdb[7] = 2;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY && p.cur == 17359 && p.stop == 45000, "48 track 2: cur %u stop %u", p.cur, p.stop);
	uint32_t gen0 = p.flush_gen;
	int n = 0;
	while (mac_cd_play_frame(&p, &lba)) n++;
	CHECK(n == 45000 - 17359 && p.state == MAC_CD_ST_END && p.cur == 45000, "ran to the end: %d frames, st %d", n, p.state);
	CHECK(mac_cd_play_ast(&p) == 3, "ast end");

	// PLAY MSF FF:FF:FF resumes from the current position; PAUSE/RESUME
	memset(cdb, 0, 12); cdb[0] = 0x47; cdb[3] = cdb[4] = cdb[5] = 0xFF; cdb[6] = 66; cdb[7] = 40; cdb[8] = 0;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY && p.cur == 45000 && p.stop == 299850 && p.flush_gen == gen0 + 1, "47 from current (66:40:00 = 299850)");
	memset(cdb, 0, 12); cdb[0] = 0x4B;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PAUSE && mac_cd_play_frame(&p, &lba) == 0 && mac_cd_play_ast(&p) == 1, "4B pause");
	cdb[8] = 1;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY, "4B resume");
	memset(cdb, 0, 12); cdb[0] = 0xCA; cdb[1] = 0x10;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PAUSE, "CA pause");
	cdb[1] = 0;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY, "CA resume");

	// PLAY AUDIO(10) length 0 = seek only, state held
	memset(cdb, 0, 12); cdb[0] = 0x45; cdb[4] = 0x88; cdb[5] = 0x28;   // lba 0x8828 = 34856
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY && p.cur == 34856 && p.stop == 299850, "45 seek-only: st %d cur %u", p.state, p.cur);
	cdb[8] = 10;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY && p.stop == 34866, "45 length 10");

	// STOP, then SCAN forward and rewind
	memset(cdb, 0, 12); cdb[0] = 0x4E;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_IDLE && mac_cd_play_ast(&p) == 5, "4E stop");
	memset(cdb, 0, 12); cdb[0] = 0xCD; cdb[9] = 0x40; cdb[3] = 10; cdb[4] = 2; cdb[5] = 0;   // 10:02:00 +150 = 45000
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY && p.scan == 1 && p.scan_dir == 0 && p.cur == 45000 && p.stop == 300000, "CD scan forward");
	CHECK(mac_cd_play_frame(&p, &lba) && lba == 45000 && p.cur == 45008 && mac_cd_play_frame(&p, &lba) && lba == 45001 && p.cur == 45016, "scan: frames sequential, playhead +8");
	cdb[1] = 0x10; cdb[9] = 0x00; cdb[2] = cdb[3] = 0; cdb[4] = 0; cdb[5] = 12;   // LBA 12, rewind
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.scan_dir == 1 && p.cur == 12, "CD rewind");
	mac_cd_play_frame(&p, &lba); mac_cd_play_frame(&p, &lba);
	CHECK(p.cur == 0 && p.state == MAC_CD_ST_PLAY, "rewind clamps at the disc start: cur %u", p.cur);
	memset(cdb, 0, 12); cdb[0] = 0xCB; cdb[9] = 0x80; cdb[5] = 0x02;   // STOP at start of track 2
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.scan == 0 && p.stop == 45000, "CB track form sets the stop: %u", p.stop);

	// SEARCH with play bit, then eject stops it
	memset(cdb, 0, 12); cdb[0] = 0xC8; cdb[1] = 0x10; cdb[9] = 0x40; cdb[5] = 0x22; cdb[6] = 0x29; cdb[7] = 0x59;
	mac_cd_play_command(&p, cdb, NULL, 0);
	CHECK(p.state == MAC_CD_ST_PLAY && p.cur == 101234 && p.stop == 300000, "C8 MSF form: cur %u stop %u", p.cur, p.stop);
	mac_cd_play_stop(&p);
	CHECK(p.state == MAC_CD_ST_IDLE && mac_cd_play_frame(&p, &lba) == 0, "stop");

	// MODE SELECT lists: descriptor must fit and say 2048; page $0E ports
	uint8_t list[32] = {};
	list[3] = 8; list[10] = 0x08; list[4 + 8] = 0x0E; list[12 + 8] = 2; list[12 + 9] = 150; list[12 + 10] = 1; list[12 + 11] = 130;
	memset(cdb, 0, 12); cdb[0] = 0x15; cdb[4] = 24;
	mac_cd_play_command(&p, cdb, list, 24);
	CHECK(p.ports[0] == 2 && p.ports[1] == 150 && p.ports[2] == 1 && p.ports[3] == 130, "MODE SELECT with descriptor");
	list[10] = 0x02;
	list[12 + 9] = 7;
	mac_cd_play_command(&p, cdb, list, 24);
	CHECK(p.ports[1] == 150, "MODE SELECT 512-byte descriptor refused");
	list[10] = 0x08; cdb[4] = 8;
	mac_cd_play_command(&p, cdb, list, 8);
	CHECK(p.ports[1] == 150, "MODE SELECT descriptor beyond the list refused");
	uint8_t list2[16] = {};
	list2[4] = 0x0E; list2[12] = 1; list2[13] = 255; list2[14] = 2; list2[15] = 255;
	cdb[4] = 16;
	mac_cd_play_command(&p, cdb, list2, 16);
	CHECK(p.ports[0] == 1 && p.ports[1] == 255 && p.ports[3] == 255, "MODE SELECT without descriptor");

	// volume / routing: unity is a passthrough, swapped channels, mute
	int16_t pcm[4] = { 1000, -2000, 32767, -32768 };
	mac_cd_play_scale(&p, pcm, 2);
	CHECK(pcm[0] == 1000 && pcm[1] == -2000 && pcm[2] == 32767 && pcm[3] == -32768, "scale unity");
	p.ports[0] = 2; p.ports[2] = 1;
	mac_cd_play_scale(&p, pcm, 1);
	CHECK(pcm[0] == -2000 && pcm[1] == 1000, "scale swapped ports");
	p.ports[0] = 1; p.ports[2] = 2; p.ports[1] = 0; p.ports[3] = 200;
	int16_t pcm2[2] = { 10000, 10000 };
	mac_cd_play_scale(&p, pcm2, 1);
	CHECK(pcm2[0] == 0 && pcm2[1] == (int16_t)((10000 * 9725) >> 15), "scale mute / vol 200: %d %d", pcm2[0], pcm2[1]);
	p.ports[0] = 0; p.ports[1] = 255;
	int16_t pcm3[2] = { 10000, 10000 };
	mac_cd_play_scale(&p, pcm3, 1);
	CHECK(pcm3[0] == 0, "port channel 0 mutes");

	// machine reset restores the defaults and drops playback
	mac_cd_play_init(&p);
	CHECK(p.ports[1] == 255 && p.state == MAC_CD_ST_IDLE && p.toc == mixed, "reset");
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "gen")) return gen(argv[2]);
	if (argc >= 3 && !strcmp(argv[1], "check"))
	{
		check_rtl(argv[2]);
		if (argc >= 4) check_vol_lut(argv[3]);
		unit_tests();
		printf("%d checks, %d failures\n", checks, fails);
		return fails ? 1 : 0;
	}
	printf("usage: cdrom_resp_test gen <dir> | check <dir> [cd_vol_lut.vh]\n");
	return 2;
}
