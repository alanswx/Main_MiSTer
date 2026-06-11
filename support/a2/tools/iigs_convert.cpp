// iigs_convert — CLI wrapper around the IIgs disk-format codec, for generating
// test images (e.g. feeding the Verilator sim) and verifying conversions.
//
// Build:  make            (in this directory)
// Usage:
//   iigs_convert po2woz  <in.po>  <out.woz>   # 800K ProDOS -> 3.5" WOZ
//   iigs_convert dsk2woz <in.dsk> <out.woz>   # 140K DOS    -> 5.25" WOZ
//   iigs_convert woz2po  <in.woz> <out.po>    # 3.5" WOZ    -> 800K ProDOS
//   iigs_convert woz2dsk <in.woz> <out.dsk>   # 5.25" WOZ   -> 140K DOS
//   iigs_convert classify <in>                # print detected disk class

#include "../iigs_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *out_size)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) { fclose(f); fprintf(stderr, "%s is empty\n", path); return NULL; }
	uint8_t *b = (uint8_t *)malloc(n);
	if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); fprintf(stderr, "read error %s\n", path); return NULL; }
	fclose(f);
	*out_size = (size_t)n;
	return b;
}

static int spit(const char *path, const uint8_t *data, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "cannot write %s\n", path); return 0; }
	int ok = fwrite(data, 1, n, f) == n;
	fclose(f);
	if (!ok) fprintf(stderr, "write error %s\n", path);
	return ok;
}

static const char *clsname(DiskClass c)
{
	switch (c) {
	case DC_HDD: return "HDD";
	case DC_FLOPPY_525: return "FLOPPY_5.25";
	case DC_FLOPPY_35: return "FLOPPY_3.5";
	default: return "UNKNOWN";
	}
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <po2woz|dsk2woz|woz2po|woz2dsk|classify> <in> [out]\n", argv[0]);
		return 2;
	}
	const char *cmd = argv[1];
	const char *in  = argv[2];
	const char *out = argc > 3 ? argv[3] : NULL;

	size_t in_size = 0;
	uint8_t *buf = slurp(in, &in_size);
	if (!buf) return 1;

	int rc = 0;

	if (!strcmp(cmd, "classify")) {
		const char *dot = strrchr(in, '.');
		DiskClass c = iigs_classify(buf, in_size, dot ? dot + 1 : NULL);
		printf("%s: %s (%zu bytes)\n", in, clsname(c), in_size);
	}
	else if (!strcmp(cmd, "po2woz")) {
		if (!out) { fprintf(stderr, "need output path\n"); rc = 2; goto done; }
		if (in_size != A2_35_IMAGE_SIZE)
			fprintf(stderr, "warning: %s is %zu bytes (expected %d)\n", in, in_size, A2_35_IMAGE_SIZE);
		size_t cap = 2 * 1024 * 1024;
		uint8_t *woz = (uint8_t *)malloc(cap);
		size_t n = a2_po_to_woz35(woz, cap, buf);
		if (!n) { fprintf(stderr, "po2woz failed\n"); rc = 1; }
		else { rc = spit(out, woz, n) ? 0 : 1; if (!rc) printf("wrote %s (%zu bytes)\n", out, n); }
		free(woz);
	}
	else if (!strcmp(cmd, "dsk2woz")) {
		if (!out) { fprintf(stderr, "need output path\n"); rc = 2; goto done; }
		if (in_size != A2_525_IMAGE_SIZE)
			fprintf(stderr, "warning: %s is %zu bytes (expected %d)\n", in, in_size, A2_525_IMAGE_SIZE);
		size_t cap = 512 * 1024;
		uint8_t *woz = (uint8_t *)malloc(cap);
		size_t n = a2_dsk_to_woz525(woz, cap, buf);
		if (!n) { fprintf(stderr, "dsk2woz failed\n"); rc = 1; }
		else { rc = spit(out, woz, n) ? 0 : 1; if (!rc) printf("wrote %s (%zu bytes)\n", out, n); }
		free(woz);
	}
	else if (!strcmp(cmd, "woz2po")) {
		if (!out) { fprintf(stderr, "need output path\n"); rc = 2; goto done; }
		uint8_t *po = (uint8_t *)malloc(A2_35_IMAGE_SIZE);
		if (!a2_woz35_to_po(po, buf, in_size)) { fprintf(stderr, "woz2po failed (not a 3.5 WOZ or decode error)\n"); rc = 1; }
		else { rc = spit(out, po, A2_35_IMAGE_SIZE) ? 0 : 1; if (!rc) printf("wrote %s (%d bytes)\n", out, A2_35_IMAGE_SIZE); }
		free(po);
	}
	else if (!strcmp(cmd, "woz2dsk")) {
		if (!out) { fprintf(stderr, "need output path\n"); rc = 2; goto done; }
		uint8_t *dsk = (uint8_t *)malloc(A2_525_IMAGE_SIZE);
		if (!a2_woz525_to_dsk(dsk, buf, in_size)) { fprintf(stderr, "woz2dsk failed (not a 5.25 WOZ or decode error)\n"); rc = 1; }
		else { rc = spit(out, dsk, A2_525_IMAGE_SIZE) ? 0 : 1; if (!rc) printf("wrote %s (%d bytes)\n", out, A2_525_IMAGE_SIZE); }
		free(dsk);
	}
	else {
		fprintf(stderr, "unknown command: %s\n", cmd);
		rc = 2;
	}

done:
	free(buf);
	return rc;
}
