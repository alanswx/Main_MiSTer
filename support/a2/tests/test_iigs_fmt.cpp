// Native standalone regression tests for the IIgs disk-format codec.
//
// Build + run:  make run   (uses clang/g++, NOT the ARM cross-compiler)
// Fixtures: real disk images under artifacts_for_iigs/. Override the directory
// with argv[1] or the FIXTURES env var. Missing fixtures are skipped, not failed.

#include "../iigs_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// --------------------------------------------------------------------------
// tiny test framework
// --------------------------------------------------------------------------
static int g_pass = 0, g_fail = 0, g_skip = 0;
static const char *g_fixtures = "../../../artifacts_for_iigs";

#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n      (%s:%d)\n", __FILE__, __LINE__); } \
} while (0)

#define SKIP(...) do { g_skip++; printf("  SKIP: "); printf(__VA_ARGS__); printf("\n"); } while (0)

static void banner(const char *name) { printf("== %s ==\n", name); }

// Load a fixture file into a malloc'd buffer. Returns NULL if not found.
static uint8_t *load(const char *relname, size_t *out_size)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", g_fixtures, relname);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) { fclose(f); return NULL; }
	uint8_t *buf = (uint8_t *)malloc(n);
	if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
	fclose(f);
	if (out_size) *out_size = (size_t)n;
	return buf;
}

// Deterministic synthetic disk of given size (no RNG, reproducible).
static uint8_t *synth(size_t n)
{
	uint8_t *b = (uint8_t *)malloc(n);
	uint32_t x = 0x12345678u;
	for (size_t i = 0; i < n; i++) {
		x = x * 1664525u + 1013904223u;          // LCG
		b[i] = (uint8_t)(x >> 24);
	}
	return b;
}

// --------------------------------------------------------------------------
// CRC32
// --------------------------------------------------------------------------
static void test_crc32(void)
{
	banner("CRC32");
	// Standard "check" vector for CRC-32/ISO-HDLC.
	CHECK(woz_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u,
	      "crc32(\"123456789\") got 0x%08X", woz_crc32((const uint8_t *)"123456789", 9));
}

// --------------------------------------------------------------------------
// 2MG
// --------------------------------------------------------------------------
static void test_2mg_roundtrip(void)
{
	banner("2MG build/parse round-trip");
	const uint32_t plen = A2_35_IMAGE_SIZE;
	uint8_t *payload = synth(plen);
	uint8_t *img = (uint8_t *)malloc(64 + plen);

	size_t total = twomg_build(img, 64 + plen, payload, plen, 1);
	CHECK(total == 64 + plen, "twomg_build total=%zu", total);

	TwoMG m;
	CHECK(twomg_parse(img, total, &m) == 1, "twomg_parse failed");
	CHECK(m.format == 1, "format=%u", m.format);
	CHECK(m.data_offset == 64, "data_offset=%u", m.data_offset);
	CHECK(m.data_len == plen, "data_len=%u", m.data_len);
	CHECK(memcmp(img + m.data_offset, payload, plen) == 0, "payload mismatch");

	free(payload); free(img);
}

static void test_2mg_real(void)
{
	banner("2MG real fixtures");
	struct { const char *f; uint32_t exp_fmt; uint32_t exp_len; } cases[] = {
		{ "Arkanoid/Arkanoid.2mg", 1, A2_35_IMAGE_SIZE },
		{ "Airball/Airball.2mg",   1, A2_35_IMAGE_SIZE },
		{ "iigs_simulation/customtests/blank.2mg", 1, 2097152 },
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		size_t sz; uint8_t *b = load(cases[i].f, &sz);
		if (!b) { SKIP("%s not found", cases[i].f); continue; }
		TwoMG m;
		int ok = twomg_parse(b, sz, &m);
		CHECK(ok, "parse %s", cases[i].f);
		if (ok) {
			CHECK(m.format == cases[i].exp_fmt, "%s format=%u", cases[i].f, m.format);
			CHECK(m.data_len == cases[i].exp_len, "%s data_len=%u (file=%zu)", cases[i].f, m.data_len, sz);
			// trailing bytes: data_offset+data_len must be <= file size (the edge case)
			CHECK(m.data_offset + m.data_len <= sz, "%s payload exceeds file", cases[i].f);
		}
		free(b);
	}
}

// --------------------------------------------------------------------------
// DC42
// --------------------------------------------------------------------------
static void test_dc42_roundtrip(void)
{
	banner("DC42 build/parse round-trip");
	const uint32_t plen = A2_35_IMAGE_SIZE;
	uint8_t *payload = synth(plen);
	uint8_t *img = (uint8_t *)malloc(84 + plen);

	size_t total = dc42_build(img, 84 + plen, payload, plen, 0x24, "TestDisk");
	CHECK(total == 84 + plen, "dc42_build total=%zu", total);
	CHECK(dc42_probe(img, total) == 1, "probe own output");

	DC42 d;
	CHECK(dc42_parse(img, total, &d) == 1, "parse failed");
	CHECK(d.data_size == plen, "data_size=%u", d.data_size);
	CHECK(d.tag_size == 0, "tag_size=%u", d.tag_size);
	CHECK(d.format_byte == 0x24, "format_byte=0x%02x", d.format_byte);
	CHECK(d.data_checksum == dc42_checksum(payload, plen), "checksum mismatch");
	CHECK(memcmp(img + 84, payload, plen) == 0, "payload mismatch");

	free(payload); free(img);
}

static void test_dc42_no_false_positive(void)
{
	banner("DC42 must NOT match raw images");
	uint8_t *raw525 = synth(A2_525_IMAGE_SIZE);
	uint8_t *raw800 = synth(A2_35_IMAGE_SIZE);
	CHECK(dc42_probe(raw525, A2_525_IMAGE_SIZE) == 0, "raw 140K false-positive");
	CHECK(dc42_probe(raw800, A2_35_IMAGE_SIZE) == 0, "raw 800K false-positive");
	free(raw525); free(raw800);
}

static void test_dc42_real(void)
{
	banner("DC42 real fixtures (validates checksum algorithm)");
	const char *files[] = { "Blank800K.img", "Blank400K.img" };
	for (size_t i = 0; i < 2; i++) {
		size_t sz; uint8_t *b = load(files[i], &sz);
		if (!b) { SKIP("%s not found", files[i]); continue; }
		DC42 d;
		int ok = dc42_parse(b, sz, &d);
		CHECK(ok, "probe/parse %s", files[i]);
		if (ok) {
			// The stored dataChecksum must match our algorithm over the payload.
			uint32_t calc = dc42_checksum(b + 84, d.data_size);
			CHECK(calc == d.data_checksum,
			      "%s dataChecksum: stored=0x%08X calc=0x%08X", files[i], d.data_checksum, calc);
		}
		free(b);
	}
}

// --------------------------------------------------------------------------
// WOZ disk type
// --------------------------------------------------------------------------
static void test_woz_type_real(void)
{
	banner("WOZ disk-type detection (real)");
	struct { const char *f; int exp; } cases[] = {
		{ "Zany Golf.woz", 2 },             // 3.5"
		{ "Zork I r15-UG3AU5.woz", 1 },     // 5.25"
	};
	for (size_t i = 0; i < 2; i++) {
		size_t sz; uint8_t *b = load(cases[i].f, &sz);
		if (!b) { SKIP("%s not found", cases[i].f); continue; }
		int t = woz_disk_type(b, sz);
		CHECK(t == cases[i].exp, "%s woz_disk_type=%d (want %d)", cases[i].f, t, cases[i].exp);
		free(b);
	}
}

// --------------------------------------------------------------------------
// sector order
// --------------------------------------------------------------------------
static void test_order_roundtrip(void)
{
	banner("DOS <-> ProDOS order round-trip");
	uint8_t *dos = synth(A2_525_IMAGE_SIZE);
	uint8_t *po  = (uint8_t *)malloc(A2_525_IMAGE_SIZE);
	uint8_t *back = (uint8_t *)malloc(A2_525_IMAGE_SIZE);
	a2_dos_to_prodos(po, dos);
	a2_prodos_to_dos(back, po);
	CHECK(memcmp(dos, back, A2_525_IMAGE_SIZE) == 0, "dos->po->dos not identity");
	// And ensure the reorder actually permutes (po != dos).
	CHECK(memcmp(dos, po, A2_525_IMAGE_SIZE) != 0, "order conversion was a no-op");
	free(dos); free(po); free(back);
}

// --------------------------------------------------------------------------
// 5.25" GCR: DSK <-> NIB
// --------------------------------------------------------------------------
static void test_dsk_nib_roundtrip(void)
{
	banner("DSK <-> NIB round-trip");
	uint8_t *dsk = synth(A2_525_IMAGE_SIZE);
	uint8_t *nib = (uint8_t *)malloc(A2_NIB_IMAGE_SIZE);
	uint8_t *back = (uint8_t *)malloc(A2_525_IMAGE_SIZE);

	a2_dsk_to_nib(nib, dsk);
	int ok = a2_nib_to_dsk(back, nib);
	CHECK(ok, "nib->dsk reported missing sectors");
	CHECK(memcmp(dsk, back, A2_525_IMAGE_SIZE) == 0, "dsk->nib->dsk not identity");
	free(dsk); free(nib); free(back);
}

static void test_dsk_nib_real(void)
{
	banner("DSK <-> NIB round-trip (real .dsk)");
	size_t sz; uint8_t *dsk = load("Apple DOS 3.3 January 1983.dsk", &sz);
	if (!dsk) { SKIP("real .dsk not found"); return; }
	if (sz != A2_525_IMAGE_SIZE) { SKIP("real .dsk size=%zu unexpected", sz); free(dsk); return; }
	uint8_t *nib = (uint8_t *)malloc(A2_NIB_IMAGE_SIZE);
	uint8_t *back = (uint8_t *)malloc(A2_525_IMAGE_SIZE);
	a2_dsk_to_nib(nib, dsk);
	CHECK(a2_nib_to_dsk(back, nib), "real nib->dsk missing sectors");
	CHECK(memcmp(dsk, back, A2_525_IMAGE_SIZE) == 0, "real dsk->nib->dsk not identity");
	free(dsk); free(nib); free(back);
}

// --------------------------------------------------------------------------
// 5.25" easy-WOZ: DSK <-> WOZ
// --------------------------------------------------------------------------
static void test_dsk_woz_roundtrip(void)
{
	banner("DSK <-> WOZ 5.25 round-trip");
	uint8_t *dsk = synth(A2_525_IMAGE_SIZE);
	size_t woz_cap = 256 * 1024;
	uint8_t *woz = (uint8_t *)malloc(woz_cap);
	uint8_t *back = (uint8_t *)malloc(A2_525_IMAGE_SIZE);

	size_t wsz = a2_dsk_to_woz525(woz, woz_cap, dsk);
	CHECK(wsz > 0, "woz encode failed");
	if (wsz > 0) {
		// structural checks
		CHECK(woz_disk_type(woz, wsz) == 1, "generated woz not type 5.25");
		uint32_t stored_crc = (uint32_t)woz[8] | (woz[9]<<8) | (woz[10]<<16) | ((uint32_t)woz[11]<<24);
		CHECK(stored_crc == woz_crc32(woz + 12, wsz - 12), "generated woz CRC mismatch");
		CHECK(a2_woz525_to_dsk(back, woz, wsz), "woz decode missing sectors");
		CHECK(memcmp(dsk, back, A2_525_IMAGE_SIZE) == 0, "dsk->woz->dsk not identity");
	}
	free(dsk); free(woz); free(back);
}

// --------------------------------------------------------------------------
// 3.5" easy-WOZ: PO <-> WOZ
// --------------------------------------------------------------------------
static void check_po_woz35(const uint8_t *po, const char *label)
{
	size_t cap = 2 * 1024 * 1024;
	uint8_t *woz = (uint8_t *)malloc(cap);
	uint8_t *back = (uint8_t *)malloc(A2_35_IMAGE_SIZE);

	size_t wsz = a2_po_to_woz35(woz, cap, po);
	CHECK(wsz > 0, "%s: woz35 encode failed", label);
	if (wsz > 0) {
		CHECK(woz_disk_type(woz, wsz) == 2, "%s: generated woz not type 3.5", label);
		uint32_t stored = (uint32_t)woz[8] | (woz[9]<<8) | (woz[10]<<16) | ((uint32_t)woz[11]<<24);
		CHECK(stored == woz_crc32(woz + 12, wsz - 12), "%s: woz35 CRC mismatch", label);
		int ok = a2_woz35_to_po(back, woz, wsz);
		CHECK(ok, "%s: woz35 decode missing sectors", label);
		CHECK(memcmp(po, back, A2_35_IMAGE_SIZE) == 0, "%s: po->woz->po not identity", label);
	}
	free(woz); free(back);
}

static void test_po_woz35_roundtrip(void)
{
	banner("PO <-> WOZ 3.5 round-trip (synthetic)");
	uint8_t *po = synth(A2_35_IMAGE_SIZE);
	check_po_woz35(po, "synthetic");
	free(po);
}

static void test_po_woz35_real(void)
{
	banner("PO <-> WOZ 3.5 round-trip (real System.Disk.po)");
	size_t sz; uint8_t *po = load("System.Disk.po", &sz);
	if (!po) { SKIP("System.Disk.po not found"); return; }
	if (sz != A2_35_IMAGE_SIZE) { SKIP("System.Disk.po size=%zu unexpected", sz); free(po); return; }
	check_po_woz35(po, "System.Disk");
	free(po);
}

// --------------------------------------------------------------------------
// per-track decode (write-back building blocks)
// --------------------------------------------------------------------------
static void test_per_track_decode(void)
{
	banner("per-track decode matches whole-disk decode");

	// 3.5: build woz, decode track-by-track, compare to whole-disk decode.
	{
		uint8_t *po = synth(A2_35_IMAGE_SIZE);
		uint8_t *woz = (uint8_t *)malloc(2 * 1024 * 1024);
		size_t wsz = a2_po_to_woz35(woz, 2 * 1024 * 1024, po);
		uint8_t *whole = (uint8_t *)calloc(1, A2_35_IMAGE_SIZE);
		uint8_t *piece = (uint8_t *)calloc(1, A2_35_IMAGE_SIZE);
		a2_woz35_to_po(whole, woz, wsz);
		for (int nt = 0; nt < 160; nt++) {
			int base = -1, cnt = -1;
			a2_woz35_decode_track(woz, wsz, nt, piece, &base, &cnt);
			CHECK(base >= 0 && cnt > 0, "3.5 track %d base/count", nt);
		}
		CHECK(memcmp(whole, piece, A2_35_IMAGE_SIZE) == 0, "3.5 per-track != whole");
		CHECK(memcmp(po, piece, A2_35_IMAGE_SIZE) == 0, "3.5 per-track != source");
		// LBA mapping: first data block (block 3) belongs to track 0.
		CHECK(a2_woz_track_for_lba(woz, wsz, 3) == 0, "3.5 lba->track");
		free(po); free(woz); free(whole); free(piece);
	}

	// 5.25: same idea.
	{
		uint8_t *dsk = synth(A2_525_IMAGE_SIZE);
		uint8_t *woz = (uint8_t *)malloc(512 * 1024);
		size_t wsz = a2_dsk_to_woz525(woz, 512 * 1024, dsk);
		uint8_t *piece = (uint8_t *)calloc(1, A2_525_IMAGE_SIZE);
		for (int t = 0; t < 35; t++)
			CHECK(a2_woz525_decode_track(woz, wsz, t, piece) == 16, "5.25 track %d sectors", t);
		CHECK(memcmp(dsk, piece, A2_525_IMAGE_SIZE) == 0, "5.25 per-track != source");
		CHECK(a2_woz_track_for_lba(woz, wsz, 3) == 0, "5.25 lba->track");
		free(dsk); free(woz); free(piece);
	}
}

// --------------------------------------------------------------------------
// write-back reconstruction (mirrors iigs_disk.cpp's per-track persist)
// --------------------------------------------------------------------------
static void test_writeback_reskew(void)
{
	banner("write-back reconstructs the source image");
	static const int D2P[16] = { 0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15 };

	// 5.25 ProDOS source: po -> dos -> woz, then decode+re-skew back to po.
	uint8_t *po = synth(A2_525_IMAGE_SIZE);
	uint8_t *dos = (uint8_t *)malloc(A2_525_IMAGE_SIZE);
	a2_prodos_to_dos(dos, po);
	uint8_t *woz = (uint8_t *)malloc(512 * 1024);
	size_t wsz = a2_dsk_to_woz525(woz, 512 * 1024, dos);

	uint8_t *recon = (uint8_t *)calloc(1, A2_525_IMAGE_SIZE);
	uint8_t *dsk = (uint8_t *)calloc(1, A2_525_IMAGE_SIZE);
	for (int t = 0; t < 35; t++) {
		a2_woz525_decode_track(woz, wsz, t, dsk);
		const uint8_t *src = dsk + t * A2_TRACK_SIZE;
		for (int s = 0; s < 16; s++)
			memcpy(recon + t * A2_TRACK_SIZE + D2P[s] * 256, src + s * 256, 256);
	}
	CHECK(memcmp(po, recon, A2_525_IMAGE_SIZE) == 0, "5.25 ProDOS write-back != source");
	free(po); free(dos); free(woz); free(recon); free(dsk);
}

// --------------------------------------------------------------------------
// classification
// --------------------------------------------------------------------------
static const char *clsname(DiskClass c)
{
	switch (c) {
	case DC_HDD: return "HDD";
	case DC_FLOPPY_525: return "5.25";
	case DC_FLOPPY_35: return "3.5";
	default: return "UNKNOWN";
	}
}

static const char *file_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot ? dot + 1 : NULL;
}

static void test_classify_real(void)
{
	banner("classify() on real fixtures");
	struct { const char *f; DiskClass exp; } cases[] = {
		{ "Live.Install.po",                       DC_HDD },
		{ "System.Disk.po",                        DC_FLOPPY_35 },   // 800K -> floppy (§4)
		{ "Zany Golf.woz",                         DC_FLOPPY_35 },
		{ "Zork I r15-UG3AU5.woz",                 DC_FLOPPY_525 },
		{ "Arkanoid/Arkanoid.2mg",                 DC_FLOPPY_35 },
		{ "iigs_simulation/customtests/blank.2mg", DC_HDD },         // 2MB HDD 2mg
		{ "Apple DOS 3.3 January 1983.dsk",        DC_FLOPPY_525 },
		{ "Apple DOS 3.3 January 1983.nib",        DC_FLOPPY_525 },
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		size_t sz; uint8_t *b = load(cases[i].f, &sz);
		if (!b) { SKIP("%s not found", cases[i].f); continue; }
		// classify only needs the header; pass a bounded view but real size.
		uint8_t head[128];
		size_t hn = sz < sizeof(head) ? sz : sizeof(head);
		memcpy(head, b, hn);
		DiskClass c = iigs_classify(head, sz, file_ext(cases[i].f));
		CHECK(c == cases[i].exp, "%s -> %s (want %s)", cases[i].f, clsname(c), clsname(cases[i].exp));
		free(b);
	}
}

// --------------------------------------------------------------------------
int main(int argc, char **argv)
{
	if (argc > 1) g_fixtures = argv[1];
	else { const char *e = getenv("FIXTURES"); if (e) g_fixtures = e; }
	printf("Fixtures dir: %s\n\n", g_fixtures);

	test_crc32();
	test_2mg_roundtrip();
	test_2mg_real();
	test_dc42_roundtrip();
	test_dc42_no_false_positive();
	test_dc42_real();
	test_woz_type_real();
	test_order_roundtrip();
	test_dsk_nib_roundtrip();
	test_dsk_nib_real();
	test_dsk_woz_roundtrip();
	test_po_woz35_roundtrip();
	test_po_woz35_real();
	test_per_track_decode();
	test_writeback_reskew();
	test_classify_real();

	printf("\n----------------------------------------\n");
	printf("PASS %d   FAIL %d   SKIP %d\n", g_pass, g_fail, g_skip);
	return g_fail ? 1 : 0;
}
