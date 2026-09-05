/*
 * sample.S — the listening clip, linked into the program.
 *
 * The ROM's ELF loader already reads FAT32, so a clip rides in the
 * image rather than this program growing a second file reader.  It
 * goes in .payload, which bench.ld places past the startup code's
 * PC-relative reach — see the linker script for why.
 *
 * The build defines AUDIO_CLIP only when the user named source
 * material.  Without it the span is empty and entry.c generates a tone
 * instead, so the default build carries no megabytes and needs no
 * one to go find a file.
 *
 * Content is what hw/tools/wav2sample.py emits: interleaved
 * little-endian 16-bit stereo, byte-for-byte what CHAN[n].FIFO takes a
 * word at a time, with the ends faded into each other so looping does
 * not click.
 */

        .section .payload,"a",@progbits
        .balign 4

        .globl  audio_clip
audio_clip:
#ifdef AUDIO_CLIP
        .incbin "audio_sample.bin"
#endif
        .globl  audio_clip_end
audio_clip_end:
