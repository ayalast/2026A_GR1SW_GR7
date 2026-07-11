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

// Pool de SFX one-shot
static const int SFX_POOL = 4;
static ma_sound  g_sfx[SFX_POOL];
static bool      g_sfxReady[SFX_POOL] = {};
static int       g_sfxNext = 0;

// Loop de proximidad
static ma_sound  g_prox;
static bool      g_proxReady = false;
static bool      g_proxPlaying = false;

// Alarma modo admiracion
static ma_sound  g_admiracion;
static bool      g_admiracionReady = false;
static bool      g_admiracionActive = false;
static bool      g_admiracionPlaying = false;
static char      g_admiracionPath[512] = {};
static float     g_admiracionSilenceLeft = 0.0f;
static const float g_admiracionBaseVol = 0.40f;

// Ambient buzz (modo normal)
static ma_sound  g_buzz;
static bool      g_buzzReady = false;
static bool      g_buzzActive = false;
static bool      g_buzzPlaying = false;
static char      g_buzzPath[512] = {};
static const float g_buzzBaseVol = 0.30f;

static char  g_path[512] = {};
static float g_menuVolume = 0.045f;
static float g_gameVolume = 0.0225f;

// Mezcla UI (0..1)
static float g_masterVol = 1.0f;
static float g_musicVol  = 1.0f;
static float g_sfxVol    = 1.0f;

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float effectiveSfx(float localVol)
{
    return clamp01(localVol) * g_masterVol * g_sfxVol;
}

static void applyBgmVolumeNow(float baseLinear)
{
    if (!g_soundReady)
        return;
    float v = clamp01(baseLinear) * g_masterVol * g_musicVol;
    ma_sound_set_volume(&g_bgm, v);
}

enum class BgmMode
{
    Off,
    MenuLoop,
    GameAmbient
};
static BgmMode g_mode = BgmMode::Off;
static bool g_blackoutMute = false;

enum class AmbientPhase
{
    Silent,
    Playing,
    FadingOut
};
static AmbientPhase g_ambientPhase = AmbientPhase::Silent;
static float g_ambientTimer = 0.0f;
static float g_ambientNext  = 0.0f;
static float g_fadeOutDuration = 4.5f;
static float g_fadeOutTimer = 0.0f;
static float g_fadeOutStartVol = 0.0f;

// Telefono lejano
static char  g_phonePath[512] = {};
static float g_phoneSilenceLeft = 20.0f;
static float g_phoneRingGapLeft = 0.0f;
static int   g_phoneRingsLeft = 0;
static const float g_phoneVolume = 0.0045f;
static const float g_phoneRingGap = 2.8f;

// Voces lejanas (Huama). RMS de archivo ~3-5x el del buzz; vol local ~0.22
// suena al nivel del zumbido (0.30) o ligeramente por encima en juego.
static char  g_voicePaths[3][512] = {};
static int   g_voiceCount = 0;
static float g_voiceSilenceLeft = 12.0f;
static const float g_voiceVolume = 0.22f;

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
    g_fadeOutTimer = 0.0f;
}

static void schedulePlayBurst()
{
    g_ambientPhase = AmbientPhase::Playing;
    g_ambientTimer = 0.0f;
    // Duracion de cada racha de musica
    g_ambientNext  = randRange(120.0f, 185.0f);
    g_fadeOutTimer = 0.0f;
}

static void beginFadeOut()
{
    if (!g_soundReady)
    {
        scheduleSilence();
        return;
    }
    g_ambientPhase = AmbientPhase::FadingOut;
    g_fadeOutTimer = 0.0f;
    g_fadeOutStartVol = g_gameVolume;
    // Fade un poco mas largo si la racha fue larga (suave)
    g_fadeOutDuration = randRange(3.5f, 5.5f);
    applyBgmVolumeNow(g_fadeOutStartVol);
    std::cout << "[Audio] Fade-out ambient ~" << g_fadeOutDuration << "s\n";
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

    volume = clamp01(volume);

    ma_sound_set_looping(&g_bgm, looping);
    ma_sound_set_volume(&g_bgm, volume * g_masterVol * g_musicVol);

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
    if (g_admiracionReady)
    {
        ma_sound_stop(&g_admiracion);
        ma_sound_uninit(&g_admiracion);
        g_admiracionReady = false;
        g_admiracionPlaying = false;
        g_admiracionActive = false;
    }
    if (g_buzzReady)
    {
        ma_sound_stop(&g_buzz);
        ma_sound_uninit(&g_buzz);
        g_buzzReady = false;
        g_buzzPlaying = false;
        g_buzzActive = false;
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
    g_menuVolume = clamp01(volume);
    g_gameVolume = g_menuVolume * 0.5f;

    if (!ensureSoundLoaded())
        return false;

    ma_sound_set_looping(&g_bgm, MA_TRUE);
    ma_sound_set_volume(&g_bgm, g_menuVolume * g_masterVol * g_musicVol);

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
    applyBgmVolumeNow(volume);
}

void AudioBgm_SetMasterVolume(float v)
{
    g_masterVol = clamp01(v);
    // Reaplicar BGM si suena
    if (g_soundReady && ma_sound_is_playing(&g_bgm) == MA_TRUE)
    {
        if (g_ambientPhase == AmbientPhase::FadingOut)
        {
            const float t = (g_fadeOutDuration > 1e-3f) ? (g_fadeOutTimer / g_fadeOutDuration) : 1.0f;
            const float u = (t >= 1.0f) ? 1.0f : (t * t);
            applyBgmVolumeNow(g_fadeOutStartVol * (1.0f - u));
        }
        else if (g_mode == BgmMode::MenuLoop)
            applyBgmVolumeNow(g_menuVolume);
        else
            applyBgmVolumeNow(g_gameVolume);
    }
    if (g_admiracionReady && g_admiracionPlaying)
        ma_sound_set_volume(&g_admiracion, effectiveSfx(g_admiracionBaseVol));
    if (g_buzzReady && g_buzzPlaying)
        ma_sound_set_volume(&g_buzz, effectiveSfx(g_buzzBaseVol));
}

void AudioBgm_SetMusicVolume(float v)
{
    g_musicVol = clamp01(v);
    AudioBgm_SetMasterVolume(g_masterVol); // reusa reaplicacion
}

void AudioBgm_SetSfxVolume(float v)
{
    g_sfxVol = clamp01(v);
    if (g_admiracionReady && g_admiracionPlaying)
        ma_sound_set_volume(&g_admiracion, effectiveSfx(g_admiracionBaseVol));
    if (g_buzzReady && g_buzzPlaying)
        ma_sound_set_volume(&g_buzz, effectiveSfx(g_buzzBaseVol));
}

float AudioBgm_GetMasterVolume() { return g_masterVol; }
float AudioBgm_GetMusicVolume()  { return g_musicVol; }
float AudioBgm_GetSfxVolume()    { return g_sfxVol; }

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
    // Si ya estamos en blackout, no arrancar rachas
    if (g_blackoutMute)
        g_ambientNext = 1.0e9f;

    std::cout << "[Audio] Modo juego: vol~" << g_gameVolume
              << " + rachas aleatorias (silencio primero ~"
              << g_ambientNext << "s)\n";
}

void AudioBgm_SetBlackoutMute(bool mute)
{
    g_blackoutMute = mute;
    if (mute)
    {
        // Corte inmediato de la musica (modo terror / luces off)
        stopSoundSoft();
        g_ambientPhase = AmbientPhase::Silent;
        g_ambientTimer = 0.0f;
        g_ambientNext = 1.0e9f; // no reprogramar rachas
        g_fadeOutTimer = 0.0f;
        std::cout << "[Audio] BGM mute blackout (silencio total)\n";
    }
    else
    {
        // Al prender luces: silencio corto y luego rachas normales
        if (g_mode == BgmMode::GameAmbient)
        {
            scheduleSilence();
            g_ambientNext = randRange(6.0f, 20.0f);
        }
        std::cout << "[Audio] BGM unmute (luces on)\n";
    }
}

void AudioBgm_EnterMenuLoop()
{
    if (g_path[0] == '\0')
        return;

    g_mode = BgmMode::MenuLoop;
    startSound(g_menuVolume, MA_TRUE, false); // menu: desde el inicio
    std::cout << "[Audio] Modo menu: loop continuo vol=" << g_menuVolume << "\n";
}

void AudioBgm_PhoneRingSetPath(const char* path)
{
    if (!path || !path[0])
    {
        g_phonePath[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(g_phonePath, sizeof(g_phonePath), path, _TRUNCATE);
#else
    std::strncpy(g_phonePath, path, sizeof(g_phonePath) - 1);
    g_phonePath[sizeof(g_phonePath) - 1] = '\0';
#endif
    g_phoneSilenceLeft = randRange(25.0f, 55.0f);
    g_phoneRingGapLeft = 0.0f;
    g_phoneRingsLeft = 0;
    std::cout << "[Audio] Telefono lejano: " << g_phonePath
              << " (5 rings/llamada, vol=" << g_phoneVolume << ")\n";
}

void AudioBgm_PhoneRingReset()
{
    g_phoneSilenceLeft = randRange(30.0f, 70.0f);
    g_phoneRingGapLeft = 0.0f;
    g_phoneRingsLeft = 0;
}

void AudioBgm_DistantVoicesSet(const char* path0, const char* path1, const char* path2)
{
    g_voiceCount = 0;
    const char* paths[3] = { path0, path1, path2 };
    for (int i = 0; i < 3; ++i)
    {
        g_voicePaths[i][0] = '\0';
        if (!paths[i] || !paths[i][0])
            continue;
#if defined(_MSC_VER)
        strncpy_s(g_voicePaths[g_voiceCount], sizeof(g_voicePaths[0]), paths[i], _TRUNCATE);
#else
        std::strncpy(g_voicePaths[g_voiceCount], paths[i], sizeof(g_voicePaths[0]) - 1);
        g_voicePaths[g_voiceCount][sizeof(g_voicePaths[0]) - 1] = '\0';
#endif
        g_voiceCount++;
    }
    // Primera voz pronto (y mas en blackout)
    g_voiceSilenceLeft = g_blackoutMute ? randRange(2.0f, 6.0f) : randRange(4.0f, 10.0f);
    std::cout << "[Audio] Voces lejanas: " << g_voiceCount
              << " pistas, vol=" << g_voiceVolume
              << " (primera en ~" << g_voiceSilenceLeft << "s)\n";
    for (int i = 0; i < g_voiceCount; ++i)
        std::cout << "  [" << i << "] " << g_voicePaths[i] << "\n";
}

void AudioBgm_DistantVoicesReset()
{
    g_voiceSilenceLeft = g_blackoutMute ? randRange(2.0f, 7.0f) : randRange(5.0f, 12.0f);
}

static void updatePhoneRing(float deltaTime)
{
    if (g_phonePath[0] == '\0')
        return;

    if (g_phoneRingsLeft > 0)
    {
        g_phoneRingGapLeft -= deltaTime;
        if (g_phoneRingGapLeft <= 0.0f)
        {
            AudioBgm_PlaySfx(g_phonePath, g_phoneVolume);
            g_phoneRingsLeft--;
            if (g_phoneRingsLeft > 0)
                g_phoneRingGapLeft = g_phoneRingGap;
            else
            {
                // Fin de llamada: silencio largo hasta la siguiente
                g_phoneSilenceLeft = randRange(50.0f, 130.0f);
                std::cout << "[Audio] Llamada terminada. Proxima en ~"
                          << g_phoneSilenceLeft << "s\n";
            }
        }
        return;
    }

    g_phoneSilenceLeft -= deltaTime;
    if (g_phoneSilenceLeft <= 0.0f)
    {
        // 4-6 rings (default 5) = llamada sin contestar
        g_phoneRingsLeft = 4 + (std::rand() % 3); // 4, 5 o 6
        g_phoneRingGapLeft = 0.0f; // primer ring ya
        std::cout << "[Audio] Telefono lejano: " << g_phoneRingsLeft << " rings\n";
    }
}

static void updateDistantVoices(float deltaTime)
{
    if (g_voiceCount <= 0)
        return;

    g_voiceSilenceLeft -= deltaTime;
    if (g_voiceSilenceLeft > 0.0f)
        return;

    const int idx = std::rand() % g_voiceCount;
    // En blackout (sin luces) un poco mas fuertes y mucho mas frecuentes
    const float vol = g_blackoutMute ? (g_voiceVolume * 1.15f) : g_voiceVolume;
    if (!AudioBgm_PlaySfx(g_voicePaths[idx], vol))
        std::cout << "[Audio] FALLO voz lejana: " << g_voicePaths[idx] << "\n";
    else
        std::cout << "[Audio] Voz lejana #" << (idx + 1)
                  << (g_blackoutMute ? " (blackout)" : "") << " OK\n";

    // Antes: 28-85s (casi inaudibles en sesion). Ahora: frecuentes; blackout casi continuo.
    if (g_blackoutMute)
        g_voiceSilenceLeft = randRange(3.0f, 9.0f);
    else
        g_voiceSilenceLeft = randRange(6.0f, 16.0f);
}

static void applyAdmiracionVolume()
{
    if (!g_admiracionReady)
        return;
    ma_sound_set_volume(&g_admiracion, effectiveSfx(g_admiracionBaseVol));
}

static bool ensureAdmiracionLoaded()
{
    if (g_admiracionPath[0] == '\0')
        return false;
    if (g_admiracionReady)
        return true;
    if (!g_engineReady)
    {
        if (!AudioBgm_Init())
            return false;
    }
    ma_result result = ma_sound_init_from_file(
        &g_engine,
        g_admiracionPath,
        MA_SOUND_FLAG_STREAM,
        NULL,
        NULL,
        &g_admiracion);
    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo cargar alarma admiracion: " << g_admiracionPath
                  << " (codigo " << (int)result << ")\n";
        return false;
    }
    ma_sound_set_looping(&g_admiracion, MA_FALSE);
    applyAdmiracionVolume();
    g_admiracionReady = true;
    g_admiracionPlaying = false;
    std::cout << "[Audio] Alarma admiracion cargada vol_base=" << g_admiracionBaseVol
              << " -> " << g_admiracionPath << "\n";
    return true;
}

static void startAdmiracionClip()
{
    if (!ensureAdmiracionLoaded())
        return;
    applyAdmiracionVolume();
    ma_sound_seek_to_pcm_frame(&g_admiracion, 0);
    ma_result result = ma_sound_start(&g_admiracion);
    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo start alarma admiracion (" << (int)result << ")\n";
        g_admiracionPlaying = false;
        return;
    }
    g_admiracionPlaying = true;
    g_admiracionSilenceLeft = 0.0f;
}

static void stopAdmiracionClip()
{
    if (!g_admiracionReady)
        return;
    ma_sound_stop(&g_admiracion);
    g_admiracionPlaying = false;
}

static void updateAdmiracionAlarm(float deltaTime)
{
    if (!g_admiracionActive)
        return;
    if (g_admiracionPath[0] == '\0')
        return;

    if (g_admiracionPlaying)
    {
        // Clip termino -> silencio aleatorio, luego otra vez
        if (g_admiracionReady && ma_sound_is_playing(&g_admiracion) != MA_TRUE)
        {
            g_admiracionPlaying = false;
            g_admiracionSilenceLeft = randRange(4.0f, 14.0f);
            std::cout << "[Audio] Alarma admiracion: silencio ~"
                      << g_admiracionSilenceLeft << "s\n";
        }
        return;
    }

    g_admiracionSilenceLeft -= deltaTime;
    if (g_admiracionSilenceLeft <= 0.0f)
        startAdmiracionClip();
}

void AudioBgm_AdmiracionAlarmSetPath(const char* path)
{
    if (!path || !path[0])
    {
        g_admiracionPath[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(g_admiracionPath, sizeof(g_admiracionPath), path, _TRUNCATE);
#else
    std::strncpy(g_admiracionPath, path, sizeof(g_admiracionPath) - 1);
    g_admiracionPath[sizeof(g_admiracionPath) - 1] = '\0';
#endif
    // Si ya estaba cargado con otro path, liberar para recargar
    if (g_admiracionReady)
    {
        ma_sound_stop(&g_admiracion);
        ma_sound_uninit(&g_admiracion);
        g_admiracionReady = false;
        g_admiracionPlaying = false;
    }
    std::cout << "[Audio] Alarma admiracion path: " << g_admiracionPath
              << " (vol_base=" << g_admiracionBaseVol << ")\n";
}

void AudioBgm_AdmiracionAlarmSetActive(bool active)
{
    if (g_admiracionActive == active)
        return;
    g_admiracionActive = active;
    if (active)
    {
        // Entra: suena ya (sin silencio inicial)
        startAdmiracionClip();
        std::cout << "[Audio] Alarma admiracion ON\n";
    }
    else
    {
        stopAdmiracionClip();
        g_admiracionSilenceLeft = 0.0f;
        std::cout << "[Audio] Alarma admiracion OFF\n";
    }
}

bool AudioBgm_AdmiracionAlarmIsPlaying()
{
    return g_admiracionActive && g_admiracionPlaying
        && g_admiracionReady
        && (ma_sound_is_playing(&g_admiracion) == MA_TRUE);
}

static bool ensureBuzzLoaded()
{
    if (g_buzzPath[0] == '\0')
        return false;
    if (g_buzzReady)
        return true;
    if (!g_engineReady)
    {
        if (!AudioBgm_Init())
            return false;
    }
    ma_result result = ma_sound_init_from_file(
        &g_engine,
        g_buzzPath,
        MA_SOUND_FLAG_STREAM,
        NULL,
        NULL,
        &g_buzz);
    if (result != MA_SUCCESS)
    {
        std::cout << "[Audio] No se pudo cargar ambient buzz: " << g_buzzPath
                  << " (codigo " << (int)result << ")\n";
        return false;
    }
    ma_sound_set_looping(&g_buzz, MA_TRUE);
    ma_sound_set_volume(&g_buzz, effectiveSfx(g_buzzBaseVol));
    g_buzzReady = true;
    g_buzzPlaying = false;
    std::cout << "[Audio] Ambient buzz cargado vol_base=" << g_buzzBaseVol
              << " -> " << g_buzzPath << "\n";
    return true;
}

void AudioBgm_AmbientBuzzSetPath(const char* path)
{
    if (!path || !path[0])
    {
        g_buzzPath[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(g_buzzPath, sizeof(g_buzzPath), path, _TRUNCATE);
#else
    std::strncpy(g_buzzPath, path, sizeof(g_buzzPath) - 1);
    g_buzzPath[sizeof(g_buzzPath) - 1] = '\0';
#endif
    if (g_buzzReady)
    {
        ma_sound_stop(&g_buzz);
        ma_sound_uninit(&g_buzz);
        g_buzzReady = false;
        g_buzzPlaying = false;
    }
    std::cout << "[Audio] Ambient buzz path: " << g_buzzPath
              << " (vol_base=" << g_buzzBaseVol << ")\n";
}

void AudioBgm_AmbientBuzzSetActive(bool active)
{
    if (g_buzzActive == active && g_buzzPlaying == active)
    {
            if (active && g_buzzReady && g_buzzPlaying)
            ma_sound_set_volume(&g_buzz, effectiveSfx(g_buzzBaseVol));
        return;
    }
    g_buzzActive = active;
    if (active)
    {
        if (!ensureBuzzLoaded())
        {
            g_buzzActive = false;
            return;
        }
        ma_sound_set_volume(&g_buzz, effectiveSfx(g_buzzBaseVol));
        if (!g_buzzPlaying)
        {
            ma_sound_seek_to_pcm_frame(&g_buzz, 0);
            ma_result r = ma_sound_start(&g_buzz);
            if (r == MA_SUCCESS)
            {
                g_buzzPlaying = true;
                std::cout << "[Audio] Ambient buzz ON (vol=" << effectiveSfx(g_buzzBaseVol) << ")\n";
            }
            else
            {
                std::cout << "[Audio] No se pudo start ambient buzz (" << (int)r << ")\n";
                g_buzzPlaying = false;
                g_buzzActive = false;
            }
        }
    }
    else
    {
        if (g_buzzReady)
        {
            ma_sound_stop(&g_buzz);
            g_buzzPlaying = false;
        }
        std::cout << "[Audio] Ambient buzz OFF\n";
    }
}

void AudioBgm_Update(float deltaTime, bool gameActive)
{
    if (deltaTime < 0.0f)
        deltaTime = 0.0f;
    if (deltaTime > 0.25f)
        deltaTime = 0.25f;

    updateAdmiracionAlarm(deltaTime);

    if (gameActive)
    {
        updatePhoneRing(deltaTime);
        updateDistantVoices(deltaTime);
    }

    if (g_mode != BgmMode::GameAmbient)
        return;

    if (g_blackoutMute)
    {
        if (g_soundReady && ma_sound_is_playing(&g_bgm) == MA_TRUE)
            stopSoundSoft();
        return;
    }

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
    else if (g_ambientPhase == AmbientPhase::Playing)
    {
        const bool stillOn = g_soundReady && (ma_sound_is_playing(&g_bgm) == MA_TRUE);
        // Iniciar fade un poco antes del fin de racha, o si el stream se corto
        const float fadeLead = g_fadeOutDuration;
        if (!stillOn)
        {
            scheduleSilence();
            std::cout << "[Audio] Silencio ambient ~" << g_ambientNext << "s\n";
        }
        else if (g_ambientTimer >= (g_ambientNext - fadeLead))
        {
            beginFadeOut();
        }
    }
    else if (g_ambientPhase == AmbientPhase::FadingOut)
    {
        g_fadeOutTimer += deltaTime;
        const float t = (g_fadeOutDuration > 1e-3f)
            ? (g_fadeOutTimer / g_fadeOutDuration)
            : 1.0f;
        // Curva suave (ease-out): baja lento al principio, mas rapido al final
        const float u = (t >= 1.0f) ? 1.0f : (t * t);
        const float vol = g_fadeOutStartVol * (1.0f - u);
        if (g_soundReady)
            applyBgmVolumeNow((vol > 0.0f) ? vol : 0.0f);

        if (t >= 1.0f || !g_soundReady || ma_sound_is_playing(&g_bgm) != MA_TRUE)
        {
            stopSoundSoft();
            scheduleSilence();
            std::cout << "[Audio] Silencio ambient (tras fade) ~" << g_ambientNext << "s\n";
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

    volume = effectiveSfx(volume);
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
    else if (std::strstr(path, "voice_distant") != nullptr)
    {
        // Un poco mas grave y variable (mas inquietante a lo lejos)
        const float pitch = 0.78f + randRange(0.0f, 0.14f); // 0.78 .. 0.92
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

    volume = effectiveSfx(volume);

    const float startThreshold = 0.0004f;

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
