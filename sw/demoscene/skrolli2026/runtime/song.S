/*
 * song.S — the song blob from tools/mod2song.py.  The build defines
 * SONG only when given a module; without one the span is empty and
 * rt_music_start() panics.
 */

        .section .payload,"a",@progbits
        .balign 4

        .globl  song_start
song_start:
#ifdef SONG
        .incbin "song.bin"
#endif
        .globl  song_end
song_end:
