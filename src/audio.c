/*
 * audio.c - looping soundtrack playback via SDL_mixer.
 */
#include "audio.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <stdio.h>

#define SOUNDTRACK_PATH "assets/soundtrack.ogg"
#define MUSIC_VOLUME 0.6f

static Mix_Music *s_music = NULL;
static int s_mixer_init = 0;
static int s_audio_open = 0;

void audio_set_music_volume(float volume)
{
    int mix_volume;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    mix_volume = (int)(volume * (float)MIX_MAX_VOLUME + 0.5f);
    Mix_VolumeMusic(mix_volume);
}

int audio_init(void)
{
    int mixer_flags = MIX_INIT_OGG;
    if ((Mix_Init(mixer_flags) & mixer_flags) != mixer_flags) {
        fprintf(stderr, "[Audio] Mix_Init OGG: %s\n", Mix_GetError());
        audio_shutdown();
        return 0;
    }
    s_mixer_init = 1;

    if (Mix_OpenAudio(48000, MIX_DEFAULT_FORMAT, 2, 2048) != 0) {
        fprintf(stderr, "[Audio] Mix_OpenAudio: %s\n", Mix_GetError());
        audio_shutdown();
        return 0;
    }
    s_audio_open = 1;

    s_music = Mix_LoadMUS(SOUNDTRACK_PATH);
    if (!s_music) {
        fprintf(stderr, "[Audio] soundtrack '%s': %s\n",
                SOUNDTRACK_PATH, Mix_GetError());
        audio_shutdown();
        return 0;
    }

    audio_set_music_volume(MUSIC_VOLUME);
    if (Mix_PlayMusic(s_music, -1) != 0) {
        fprintf(stderr, "[Audio] Mix_PlayMusic: %s\n", Mix_GetError());
        audio_shutdown();
        return 0;
    }

    fprintf(stdout, "[Audio] looping %s\n", SOUNDTRACK_PATH);
    return 1;
}

void audio_shutdown(void)
{
    if (s_music) {
        Mix_HaltMusic();
        Mix_FreeMusic(s_music);
        s_music = NULL;
    }

    if (s_audio_open) {
        Mix_CloseAudio();
        s_audio_open = 0;
    }

    if (s_mixer_init) {
        Mix_Quit();
        s_mixer_init = 0;
    }
}
