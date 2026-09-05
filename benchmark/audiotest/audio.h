/*
 * audio.h — CLASS_AUDIO register map.
 *
 * Mirror of doc/system/devices/audio.md.  Offsets only; the semantics
 * live in the spec.
 */

#ifndef AUDIOTEST_AUDIO_H
#define AUDIOTEST_AUDIO_H

/* ── Global registers ─────────────────────────────────────────── */
#define PA_CAP              0x000
#define PA_RATE             0x004
#define PA_CTRL             0x008
#define PA_CHAN_EN          0x00C
#define PA_IRQ_STATUS       0x010
#define PA_IRQ_ENABLE       0x014
#define PA_UNDERRUN         0x018
#define PA_MASTER_VOL       0x01C

/* ── Per-channel block ────────────────────────────────────────── */
#define PA_CHAN(n)          (0x100 + (n) * 0x20)
#define PA_C_CTRL           0x00
#define PA_C_INC            0x04
#define PA_C_VOL            0x08
#define PA_C_DEPTH          0x0C
#define PA_C_LEVEL          0x10
#define PA_C_WM             0x14
#define PA_C_FIFO           0x18

/* ── Field encodings ──────────────────────────────────────────── */
#define PA_CAP_VERSION(c)   ((c) & 0xFF)
#define PA_CAP_PITCH        (1u << 8)
#define PA_CAP_VOLUME       (1u << 9)
#define PA_CAP_FMT8         (1u << 10)
#define PA_CAP_MONO         (1u << 11)
#define PA_CAP_INTERP       (1u << 12)
#define PA_CAP_CHANNELS(c)  (((c) >> 16) & 0xFF)

#define PA_CTRL_ENABLE      (1u << 0)

#define PA_C_CTRL_FMT8      (1u << 0)
#define PA_C_CTRL_MONO      (1u << 1)
#define PA_C_CTRL_INTERP    (1u << 2)
#define PA_C_CTRL_FLUSH     (1u << 8)

/* Values the optional registers read when their capability is absent. */
#define PA_INC_UNITY        0x00010000u
#define PA_VOL_UNITY        0xFFFFFFFFu

#endif /* AUDIOTEST_AUDIO_H */
