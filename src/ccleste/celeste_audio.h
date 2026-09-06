#include <stddef.h>
#include <stdint.h>

void celeste_init_audio(void);
void celeste_api_music(int pattern, int16_t fade_len, int16_t mask);
void celeste_api_sfx(int sfx, int channel, int offset);
void celeste_fill_audio_buffer(void *audioBuffer, size_t offset, size_t size);
