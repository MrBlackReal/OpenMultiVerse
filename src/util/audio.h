/*
 * audio.h - background music playback.
 */
#pragma once

/* Initialise SDL_mixer and start the looping soundtrack if available.
 * Returns 1 when music started, 0 when audio is unavailable or the asset is
 * missing. The simulator keeps running either way. */
int audio_init(void);

/* Stop playback and release SDL_mixer resources. Safe to call even if init
 * failed or was never called. */
void audio_shutdown(void);

/* Set music volume in [0, 1]. */
void audio_set_music_volume(float volume);
