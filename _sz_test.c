#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
void dlna_cp(uint32_t s) { (void)s; }
#include "main/dlna/minimp3.h"
static mp3dec_t s_mp3_dec;
int main(void) {
    printf("sizeof(mp3dec_t)=%d dec_size=%d\n", (int)sizeof(mp3dec_t), (int)sizeof(s_mp3_dec));
    return 0;
}
