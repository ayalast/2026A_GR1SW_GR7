#pragma once

// Musica de fondo + SFX cortos (miniaudio). No bloquea el render loop.
//
// Modos BGM:
//   Menu/carga: loop continuo a volumen "lejos".
//   Juego: ~mitad de volumen + rachas aleatorias (estilo Minecraft).

bool AudioBgm_Init();
void AudioBgm_Shutdown();

// path relativo al working directory (ej. "sounds/S1.mp3")
// volume: 0.0 .. 1.0  (carga/menu ~0.045)
bool AudioBgm_PlayLoop(const char* path, float volume);

void AudioBgm_SetVolume(float volume);
void AudioBgm_Stop();
bool AudioBgm_IsPlaying();

// Al pulsar COMENZAR: baja volumen a la mitad y activa silencios/rachas.
void AudioBgm_EnterGameAmbient();
// Si vuelves al menu principal: loop continuo al volumen original.
void AudioBgm_EnterMenuLoop();

// Llamar cada frame con deltaTime (segundos).
void AudioBgm_Update(float deltaTime);

// SFX one-shot (ej. linterna on/off). Misma pista se puede re-disparar.
bool AudioBgm_PlaySfx(const char* path, float volume = 0.85f);
