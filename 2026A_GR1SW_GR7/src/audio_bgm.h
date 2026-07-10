#pragma once

// Audio de fondo y efectos (miniaudio)

bool AudioBgm_Init();
void AudioBgm_Shutdown();

// path relativo al cwd; volume se multiplica por master*music
bool AudioBgm_PlayLoop(const char* path, float volume);

void AudioBgm_SetVolume(float volume); // volumen lineal directo al BGM (interno)
void AudioBgm_Stop();
bool AudioBgm_IsPlaying();

// Sliders de volumen (0..1)
void AudioBgm_SetMasterVolume(float v);
void AudioBgm_SetMusicVolume(float v);
void AudioBgm_SetSfxVolume(float v);
float AudioBgm_GetMasterVolume();
float AudioBgm_GetMusicVolume();
float AudioBgm_GetSfxVolume();

// Entra al modo de musica por rachas (en juego)
void AudioBgm_EnterGameAmbient();
// Vuelve al loop continuo del menu
void AudioBgm_EnterMenuLoop();

// Silencia la musica en blackout
void AudioBgm_SetBlackoutMute(bool mute);

// Llamar cada frame
void AudioBgm_Update(float deltaTime, bool gameActive = false);

// SFX one-shot
bool AudioBgm_PlaySfx(const char* path, float volume = 0.85f);

// Telefono lejano
void AudioBgm_PhoneRingSetPath(const char* path);
void AudioBgm_PhoneRingReset();

// Voces lejanas
void AudioBgm_DistantVoicesSet(const char* path0, const char* path1, const char* path2);
void AudioBgm_DistantVoicesReset();

// Loop por proximidad a la entidad
bool AudioBgm_ProximityLoad(const char* path);
void AudioBgm_ProximitySetVolume(float volume);
void AudioBgm_ProximityStop();

// Alarma del modo admiracion (canal aparte)
void AudioBgm_AdmiracionAlarmSetPath(const char* path);
void AudioBgm_AdmiracionAlarmSetActive(bool active);
bool AudioBgm_AdmiracionAlarmIsPlaying();

// Zumbido de fondo en modo normal
void AudioBgm_AmbientBuzzSetPath(const char* path);
void AudioBgm_AmbientBuzzSetActive(bool active);
