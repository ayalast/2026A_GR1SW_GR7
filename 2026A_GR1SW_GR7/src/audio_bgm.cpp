#include "audio_bgm.h"

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <ctime>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

static ma_engine g_engine;
static ma_sound  g_bgm;
static bool      g_engineReady = false;
static bool      g_soundReady  = false;

// Pool de SFX one-shot (pasos + linterna sin cortarse entre si)
static const int SFX_POOL = 4;
static ma_sound  g_sfx[SFX_POOL];
static bool      g_sfxReady[SFX_POOL] = {};
static int       g_sfxNext = 0;

// Loop de proximidad (entidad): volumen se actualiza por distancia
static ma_sound  g_prox;
static bool      g_proxReady = false;
static bool      g_proxPlaying = false;

static char  g_path[512] = {};
static float g_menuVolume = 0.045f;
static float g_gameVolume = 0.0225f; // mitad del volumen de carga/menu

enum class BgmMode
{
    Off,
    MenuLoop,      // carga + menu: siempre sonando
    GameAmbient    // en juego: rachas + silencios
};
static BgmMode g_mode = BgmMode::Off;

enum class AmbientPhase
{
    Silent,
    Playing
};
static AmbientPhase g_ambientPhase = AmbientPhase::Silent;
static float g_ambientTimer = 0.0f;
static float g_ambientNext  = 0.0f;

static float randRange(float lo, float hi)
{
    const float t = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    return lo + (hi - lo) * t;
}

static void scheduleSilence()
{
    g_ambientPhase = AmbientPhase::Silent;
    g_ambientTimer = 0.0f;
    g_ambientNext  = randRange(25.0f, 90.0f);
}

static void schedulePlayBurst()
{
    g_ambientPhase = AmbientPhase::Playing;
    g_ambientTimer = 0.0f;
    // Al menos 2-3 min de cancion por racha (antes ~22-70s y siempre desde el inicio)
    g_ambientNext  = randRange(120.0f, 185.0f);
}

static bool ensureSoundLoaded()
{
    if (!g_engineReady)
    {
        if (!AudioBgm_Init())
            return false;
    }
    if (g_soundReady)
        return true;
    if (g_path[0] == '\0')
        return false;

    ma_result result = ma_sound_init_from_file(
        &g_engine,
        g_path,
        MA_SOUND_FLAG_STREAM,
        NULL,
        NULL,
        &g_bgm);

    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo cargar BGM: " << g_path
                  << " (codigo " << (int)result << ")\n";
        return false;
    }

    g_soundReady = true;
    return true;
}

static void startSound(float volume, ma_bool32 looping, bool randomOffset)
{
    if (!ensureSoundLoaded())
        return;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    ma_sound_set_looping(&g_bgm, looping);
    ma_sound_set_volume(&g_bgm, volume);

    ma_uint64 seekFrame = 0;
    if (randomOffset)
    {
        ma_uint64 lengthFrames = 0;
        if (ma_sound_get_length_in_pcm_frames(&g_bgm, &lengthFrames) == MA_SUCCESS && lengthFrames > 1)
        {
            // Empieza en un punto aleatorio (evita oir solo el primer minuto siempre).
            // No arrancar en el ultimo ~12% para que haya cuerpo de cancion por delante.
            const double maxFrac = 0.88;
            const double t = static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX);
            seekFrame = static_cast<ma_uint64>(t * maxFrac * static_cast<double>(lengthFrames));
            if (seekFrame >= lengthFrames)
                seekFrame = lengthFrames - 1;
            std::cout << "[Audio] BGM seek aleatorio frame=" << seekFrame
                      << " / " << lengthFrames << "\n";
        }
    }
    ma_sound_seek_to_pcm_frame(&g_bgm, seekFrame);

    ma_result result = ma_sound_start(&g_bgm);
    if (result != MA_SUCCESS)
        std::cout << "[Audio] No se pudo start BGM (" << (int)result << ")\n";
}

static void stopSoundSoft()
{
    if (!g_soundReady)
        return;
    ma_sound_stop(&g_bgm);
}

bool AudioBgm_Init()
{
    if (g_engineReady)
        return true;

    static bool seeded = false;
    if (!seeded)
    {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        seeded = true;
    }

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
    for (int i = 0; i < SFX_POOL; ++i)
    {
        if (g_sfxReady[i])
        {
            ma_sound_uninit(&g_sfx[i]);
            g_sfxReady[i] = false;
        }
    }
    if (g_proxReady)
    {
        ma_sound_stop(&g_prox);
        ma_sound_uninit(&g_prox);
        g_proxReady = false;
        g_proxPlaying = false;
    }
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
    g_mode = BgmMode::Off;
}

bool AudioBgm_PlayLoop(const char* path, float volume)
{
    if (!path || !path[0])
        return false;

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

#if defined(_MSC_VER)
    strncpy_s(g_path, sizeof(g_path), path, _TRUNCATE);
#else
    std::strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
#endif
    g_menuVolume = volume;
    g_gameVolume = volume * 0.5f;

    if (!ensureSoundLoaded())
        return false;

    ma_sound_set_looping(&g_bgm, MA_TRUE);
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    ma_sound_set_volume(&g_bgm, volume);

    ma_result result = ma_sound_start(&g_bgm);
    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo reproducir BGM (" << (int)result << ")\n";
        ma_sound_uninit(&g_bgm);
        g_soundReady = false;
        return false;
    }

    g_mode = BgmMode::MenuLoop;
    std::cout << "[Audio] BGM menu/carga en loop, vol=" << volume
              << " (juego usara ~" << g_gameVolume << " a rachas) -> " << path << "\n";
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
    stopSoundSoft();
}

bool AudioBgm_IsPlaying()
{
    if (!g_soundReady)
        return false;
    return ma_sound_is_playing(&g_bgm) == MA_TRUE;
}

void AudioBgm_EnterGameAmbient()
{
    if (g_path[0] == '\0')
        return;

    g_mode = BgmMode::GameAmbient;
    stopSoundSoft();
    scheduleSilence();
    g_ambientNext = randRange(4.0f, 22.0f);

    std::cout << "[Audio] Modo juego: vol~" << g_gameVolume
              << " + rachas aleatorias (silencio primero ~"
              << g_ambientNext << "s)\n";
}

void AudioBgm_EnterMenuLoop()
{
    if (g_path[0] == '\0')
        return;

    g_mode = BgmMode::MenuLoop;
    startSound(g_menuVolume, MA_TRUE, false); // menu: desde el inicio
    std::cout << "[Audio] Modo menu: loop continuo vol=" << g_menuVolume << "\n";
}

void AudioBgm_Update(float deltaTime)
{
    if (g_mode != BgmMode::GameAmbient)
        return;
    if (deltaTime < 0.0f)
        deltaTime = 0.0f;
    if (deltaTime > 0.25f)
        deltaTime = 0.25f;

    g_ambientTimer += deltaTime;

    if (g_ambientPhase == AmbientPhase::Silent)
    {
        if (g_ambientTimer >= g_ambientNext)
        {
            startSound(g_gameVolume, MA_TRUE, true); // juego: offset aleatorio en la pista
            schedulePlayBurst();
            std::cout << "[Audio] Racha ambient ~" << g_ambientNext << "s (vol="
                      << g_gameVolume << ", 2-3 min, seek random)\n";
        }
    }
    else
    {
        const bool stillOn = g_soundReady && (ma_sound_is_playing(&g_bgm) == MA_TRUE);
        if (g_ambientTimer >= g_ambientNext || !stillOn)
        {
            stopSoundSoft();
            scheduleSilence();
            std::cout << "[Audio] Silencio ambient ~" << g_ambientNext << "s\n";
        }
    }
}

bool AudioBgm_PlaySfx(const char* path, float volume)
{
    if (!path || !path[0])
        return false;

    if (!g_engineReady)
    {
        if (!AudioBgm_Init())
            return false;
    }

    const int slot = g_sfxNext;
    g_sfxNext = (g_sfxNext + 1) % SFX_POOL;

    if (g_sfxReady[slot])
    {
        ma_sound_stop(&g_sfx[slot]);
        ma_sound_uninit(&g_sfx[slot]);
        g_sfxReady[slot] = false;
    }

    ma_result result = ma_sound_init_from_file(
        &g_engine,
        path,
        0,
        NULL,
        NULL,
        &g_sfx[slot]);

    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo cargar SFX: " << path
                  << " (codigo " << (int)result << ")\n";
        return false;
    }

    g_sfxReady[slot] = true;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    ma_sound_set_looping(&g_sfx[slot], MA_FALSE);
    ma_sound_set_volume(&g_sfx[slot], volume);

    // Ligera variacion de pitch en pasos (mismo archivo, menos robotico)
    // miniaudio: ma_sound_set_pitch
    // Solo si el path parece footstep - caller can pass volume; optional pitch via path check
    if (std::strstr(path, "footstep") != nullptr)
    {
        const float pitch = 0.92f + randRange(0.0f, 0.16f); // 0.92 .. 1.08
        ma_sound_set_pitch(&g_sfx[slot], pitch);
    }

    result = ma_sound_start(&g_sfx[slot]);
    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo reproducir SFX (" << (int)result << ")\n";
        ma_sound_uninit(&g_sfx[slot]);
        g_sfxReady[slot] = false;
        return false;
    }

    return true;
}

bool AudioBgm_ProximityLoad(const char* path)
{
    if (!path || !path[0])
        return false;

    if (!g_engineReady)
    {
        if (!AudioBgm_Init())
            return false;
    }

    if (g_proxReady)
    {
        ma_sound_stop(&g_prox);
        ma_sound_uninit(&g_prox);
        g_proxReady = false;
        g_proxPlaying = false;
    }

    ma_result result = ma_sound_init_from_file(
        &g_engine,
        path,
        MA_SOUND_FLAG_STREAM,
        NULL,
        NULL,
        &g_prox);

    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo cargar proximidad: " << path
                  << " (codigo " << (int)result << ")\n";
        return false;
    }

    ma_sound_set_looping(&g_prox, MA_TRUE);
    ma_sound_set_volume(&g_prox, 0.0f);
    g_proxReady = true;
    g_proxPlaying = false;
    std::cout << "[Audio] Proximidad entidad cargada (loop, vol por distancia): " << path << "\n";
    return true;
}

void AudioBgm_ProximitySetVolume(float volume)
{
    if (!g_proxReady)
        return;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    // Umbral muy bajo: el archivo es desproporcionadamente fuerte
    const float startThreshold = 0.0008f;

    if (volume < startThreshold)
    {
        ma_sound_set_volume(&g_prox, 0.0f);
        if (g_proxPlaying)
        {
            ma_sound_stop(&g_prox);
            g_proxPlaying = false;
        }
        return;
    }

    ma_sound_set_volume(&g_prox, volume);
    if (!g_proxPlaying)
    {
        ma_sound_seek_to_pcm_frame(&g_prox, 0);
        ma_result result = ma_sound_start(&g_prox);
        if (result == MA_SUCCESS)
            g_proxPlaying = true;
        else
            std::cout << "[Audio] No se pudo start proximidad (" << (int)result << ")\n";
    }
}

void AudioBgm_ProximityStop()
{
    if (!g_proxReady)
        return;
    ma_sound_set_volume(&g_prox, 0.0f);
    ma_sound_stop(&g_prox);
    g_proxPlaying = false;
}
