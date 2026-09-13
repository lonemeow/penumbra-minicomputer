/* audio.h — CLASS_AUDIO register map (doc/system/devices/audio.md). */

#ifndef AUDIO_H
#define AUDIO_H

#define PA_CAP              0x000
#define PA_RATE             0x004
#define PA_CTRL             0x008
#define PA_CHAN_EN          0x00C
#define PA_IRQ_STATUS       0x010
#define PA_IRQ_ENABLE       0x014
#define PA_UNDERRUN         0x018
#define PA_MASTER_VOL       0x01C

#define PA_CHAN(n)          (0x100 + (n) * 0x20)
#define PA_C_CTRL           0x00
#define PA_C_INC            0x04
#define PA_C_VOL            0x08
#define PA_C_DEPTH          0x0C
#define PA_C_LEVEL          0x10
#define PA_C_WM             0x14
#define PA_C_FIFO           0x18

#define PA_CAP_PITCH        (1u << 8)
#define PA_CAP_VOLUME       (1u << 9)
#define PA_CAP_CHANNELS(c)  (((c) >> 16) & 0xFF)

#define PA_CTRL_ENABLE      (1u << 0)
#define PA_C_CTRL_FLUSH     (1u << 8)

#define AUDIO_TRACKS        4
#define AUDIO_RATE          48000

#define AU_REG(off)   (*(volatile uint32_t *)(rt_audio_base + (off)))
#define AU_CH(n, off) AU_REG(PA_CHAN(n) + (off))

#endif /* AUDIO_H */
