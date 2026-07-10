#include "audio_bgm.h"

#include <iostream>
#include <cstring>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

static ma_engine g_engine;
static ma_sound  g_bgm;
static bool      g_engineReady = false;
static bool      g_soundReady  = false;

bool AudioBgm_Init()
{
    if (g_engineReady)
        return true;

    ma_result result = ma_engine_init(NULL, &g_engine);
    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] Error al iniciar motor de audio (" << (int)result << ")\n";
        g_engineReady = false;
        return false;
    }

    g_engineReady = true;
    std::cout << "[Audio] Motor de audio listo.\n";
    return true;
}

void AudioBgm_Shutdown()
{
    if (g_soundReady)
    {
        ma_sound_uninit(&g_bgm);
        g_soundReady = false;
    }
    if (g_engineReady)
    {
        ma_engine_uninit(&g_engine);
        g_engineReady = false;
    }
}

bool AudioBgm_PlayLoop(const char* path, float volume)
{
    if (!g_engineReady)
    {
        if (!AudioBgm_Init())
            return false;
    }

    if (g_soundReady)
    {
        ma_sound_stop(&g_bgm);
        ma_sound_uninit(&g_bgm);
        g_soundReady = false;
    }

    ma_result result = ma_sound_init_from_file(
        &g_engine,
        path,
        MA_SOUND_FLAG_STREAM, // MP3 grande: streaming (no cargar 37MB enteros a RAM de golpe)
        NULL,
        NULL,
        &g_bgm);

    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo cargar BGM: " << path
                  << " (codigo " << (int)result << ")\n";
        std::cout << "[Audio] Verifica que el working directory sea la carpeta del proyecto"
                  << " (donde esta sounds/).\n";
        return false;
    }

    ma_sound_set_looping(&g_bgm, MA_TRUE);
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    ma_sound_set_volume(&g_bgm, volume);

    result = ma_sound_start(&g_bgm);
    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo reproducir BGM (" << (int)result << ")\n";
        ma_sound_uninit(&g_bgm);
        return false;
    }

    g_soundReady = true;
    std::cout << "[Audio] BGM en loop (distante), vol=" << volume << " -> " << path << "\n";
    return true;
}

void AudioBgm_SetVolume(float volume)
{
    if (!g_soundReady)
        return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    ma_sound_set_volume(&g_bgm, volume);
}

void AudioBgm_Stop()
{
    if (!g_soundReady)
        return;
    ma_sound_stop(&g_bgm);
}

bool AudioBgm_IsPlaying()
{
    if (!g_soundReady)
        return false;
    return ma_sound_is_playing(&g_bgm) == MA_TRUE;
}
