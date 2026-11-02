/* Audio: the SoC's audio DSP brought up the way stock eCos does it (audio.c, notes/audio_dsp.md), PCM out through the
 * internal DAC. Net task only. */
#ifndef AUDIO_H
#define AUDIO_H
typedef int (*aud_fmt)(char *, const char *, ...);
typedef void (*aud_logf)(const char *, ...);

int aud_init(aud_logf log);              /* clocks, codec, DSP microcode and boot, output on, master muted; 0 = ok */
int aud_ready(void);
void aud_poll(void);                     /* every net-loop pass: DSP messages, heartbeat, keep the PCM ring fed */
void aud_set_volume(int index);          /* 0..40, eCos's volume index (0 = silent, 40 = 0 dB) */
void aud_set_mute(int on);               /* DSP master mute (the MCU's system mute is separate) */
void aud_tone(int hz);                   /* test tone into the PCM ring (0 = off) */
int aud_write(const short *lr, int frames);   /* media: 48 kHz s16 stereo, little-endian; returns frames taken */
int aud_write_mono16k(int src, const short *m, int samples);   /* 1 guidance/speech, 2 system: 16 kHz mono, mixed in */
int aud_status(char *o, aud_fmt fmt);
#endif
