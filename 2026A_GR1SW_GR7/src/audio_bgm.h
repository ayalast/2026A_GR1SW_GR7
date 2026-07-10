#pragma once

// Musica de fondo lejana (loop + volumen bajo).
// Usa miniaudio. No bloquea el render loop.

bool AudioBgm_Init();
void AudioBgm_Shutdown();

// path relativo al working directory del proyecto (ej. "sounds/S1.mp3")
// volume: 0.0 .. 1.0  (para "a la distancia" usar ~0.12 - 0.22)
bool AudioBgm_PlayLoop(const char* path, float volume);
void AudioBgm_SetVolume(float volume);
void AudioBgm_Stop();
bool AudioBgm_IsPlaying();
