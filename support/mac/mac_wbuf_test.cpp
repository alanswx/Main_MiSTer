// Host test of the Mac hard-disk write buffer in user_io.cpp: extract the block from
// "#define MWB_MAX" up to user_io_flush_write_buffers() into mwb_block.inc, then
// g++ -O2 -o t mac_wbuf_test.cpp && ./t   (random streams, metadata rewrites, reads; checked against a reference image)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
struct fileTYPE { int type; uint64_t size; uint64_t offset; };
static fileTYPE sd_image[16];
static int sd_image_cangrow[16];
static std::vector<uint8_t> disk_img, ref_img;
static uint64_t fake_now = 0;
static uint64_t mdt_us() { return fake_now; }
static char is_mac_scsi_family() { return 1; }
static int mac_cdrom_slot() { return 4; }
static int mac_toolbox_slot() { return 5; }
static int mac_cd_toolbox_slot() { return -1; }
static void diskled_on() {}
static unsigned long writes_to_card = 0;
static int FileSeek(fileTYPE *f, uint64_t off, int) { f->offset = off; return off <= f->size; }
static int FileWriteAdv(fileTYPE *f, const void *d, uint32_t n) { memcpy(&disk_img[f->offset], d, n); f->offset += n; writes_to_card++; return 1; }
#define SEEK_SET 0
#include "mwb_block.inc"
int main() {
	const uint64_t SZ = 4 << 20; disk_img.assign(SZ, 0); ref_img.assign(SZ, 0);
	sd_image[0].size = SZ; srand(12345);
	uint8_t buf[64 * 1024]; unsigned long nwr = 0, nrd = 0, bad = 0;
	uint64_t seq = 0;
	for (int it = 0; it < 2000000; it++) {
		fake_now += rand() % 300;
		int r = rand() % 100;
		uint32_t blks = (rand() % 4 == 0) ? 8 : 1;
		uint64_t lba;
		if (r < 55) { lba = seq; seq += blks; if (seq * 512 + 4096 > SZ) seq = rand() % 1000; }      // sequential stream
		else if (r < 75) lba = 100 + rand() % 64;                                                   // hot metadata area
		else lba = rand() % (SZ / 512 - 8);                                                         // anywhere
		uint32_t sz = blks * 512;
		if (r < 88) {                                               // write
			for (uint32_t i = 0; i < sz; i++) buf[i] = rand();
			memcpy(&ref_img[lba * 512], buf, sz);
			if (!mwb_write(0, lba * 512, buf, sz)) { memcpy(&disk_img[lba * 512], buf, sz); }
			nwr++;
		} else if (r < 98) {                                        // read: Main flushes overlap of two buffers + sz, then reads the card
			uint64_t off = lba * 512, len = 2ULL * 16384 + sz;
			mwb_before_read(0, off, len);
			if (off + len > SZ) len = SZ - off;
			if (memcmp(&disk_img[off], &ref_img[off], len)) { bad++; if (bad < 5) printf("MISMATCH on read at %llu\n", (unsigned long long)off); }
			nrd++;
		} else {                                                    // idle gap
			fake_now += 25000; mwb_poll_test:;
			for (int d = 0; d < 1; d++) { int any = 0; for (int i = 0; i < MWB_RUNS; i++) any |= mwb[d][i].len != 0;
				if (any && fake_now - mwb_last_any[d] >= MWB_IDLE_US) mwb_flush(d); }
		}
	}
	mwb_flush(0);
	int final_ok = !memcmp(disk_img.data(), ref_img.data(), SZ);
	printf("writes %lu reads %lu read mismatches %lu final image %s; card writes %lu (%.1f guest writes per card write)\n",
	       nwr, nrd, bad, final_ok ? "IDENTICAL" : "DIFFERENT", writes_to_card, (double)nwr / writes_to_card);
	return (bad || !final_ok) ? 1 : 0;
}
