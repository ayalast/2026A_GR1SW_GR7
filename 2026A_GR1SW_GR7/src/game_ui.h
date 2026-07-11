#pragma once

// UI de menus (inicio / pausa) y reinicio rapido de la app.
// Coords normalizadas 0..1 (arriba = 0). Layout: tools/_gen_pause_menu.py

// Estados de la aplicacion (compartidos con main)
enum class AppState
{
    Menu,
    FadeIn,
    Playing,
    Paused
};

// Definidos en main.cpp
extern unsigned int SCR_WIDTH;
extern unsigned int SCR_HEIGHT;
extern AppState appState;

// Hit tests (mx/my en pixels del framebuffer)
bool uiHitButtonAt(double mx, double my, double cy, double hw = 0.12, double hh = 0.030, double cx = 0.5);
bool uiHitPrimaryButton(double mx, double my);
bool uiHitSecondaryButton(double mx, double my);
bool uiHitAdmiracionButton(double mx, double my);
bool uiHitTertiaryButton(double mx, double my);
bool uiHitQuaternaryButton(double mx, double my);

// Sliders de volumen en pausa (tracks de la textura)
struct PauseSliderLayout
{
    float trackX0;
    float trackX1;
    float sliderNy[3]; // master, music, sfx
};
PauseSliderLayout getPauseSliderLayout();

// Cierra y relanza el .exe (Windows)
void restartApplicationFast();
