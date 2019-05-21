#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define KEYMASK 0xfff
#define SHIFT 0x10000

int printable_to_key(int c);
int keycode_to_key(int k);

#ifdef __cplusplus
}
#endif



