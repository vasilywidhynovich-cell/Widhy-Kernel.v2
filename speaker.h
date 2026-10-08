#ifndef SPEAKER_H
#define SPEAKER_H
#include <stdint.h>

void speaker_play(uint32_t freq);         /* bunyi terus di freq Hz */
void speaker_stop(void);                  /* matikan */
void speaker_beep(uint32_t freq, uint32_t ms);  /* bunyi ms milidetik */

#endif
