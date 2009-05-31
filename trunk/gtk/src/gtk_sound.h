#ifndef __GTK_SOUND_H
#define __GTK_SOUND_H

void S9xPortSoundInit (void);
void S9xPortSoundDeinit (void);
void S9xPortSoundReinit (void);
void S9xSoundStart (void);
void S9xSoundStop (void);

int base2log (int num);
int powerof2 (int num);

extern int playback_rates[8];
extern int buffer_sizes[8];
extern double d_playback_rates[8];

#endif /* __GTK_SOUND_H */
