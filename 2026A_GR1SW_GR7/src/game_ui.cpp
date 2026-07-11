#include "game_ui.h"

#include "audio_bgm.h"

#include <iostream>
#include <cstdio>
#include <cstring>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

bool uiHitButtonAt(double mx, double my, double cy, double hw, double hh, double cx)
{
    if (SCR_WIDTH == 0 || SCR_HEIGHT == 0) return false;
    double nx = mx / (double)SCR_WIDTH;
    double ny = my / (double)SCR_HEIGHT;
    return nx > cx - hw && nx < cx + hw && ny > cy - hh && ny < cy + hh;
}

bool uiHitPrimaryButton(double mx, double my)
{
    if (appState == AppState::Paused)
        return uiHitButtonAt(mx, my, 0.34, 0.080, 0.025, 0.30);
    return uiHitButtonAt(mx, my, 0.546);
}

bool uiHitSecondaryButton(double mx, double my)
{
    if (appState == AppState::Paused)
        return uiHitButtonAt(mx, my, 0.42, 0.080, 0.025, 0.30);
    return uiHitButtonAt(mx, my, 0.634);
}

bool uiHitAdmiracionButton(double mx, double my)
{
    return uiHitButtonAt(mx, my, 0.50, 0.080, 0.025, 0.30);
}

bool uiHitTertiaryButton(double mx, double my)
{
    return uiHitButtonAt(mx, my, 0.58, 0.080, 0.025, 0.30);
}

bool uiHitQuaternaryButton(double mx, double my)
{
    return uiHitButtonAt(mx, my, 0.66, 0.080, 0.025, 0.30);
}

PauseSliderLayout getPauseSliderLayout()
{
    PauseSliderLayout L;
    L.trackX0 = 0.58f;
    L.trackX1 = 0.82f;
    L.sliderNy[0] = 0.38f;
    L.sliderNy[1] = 0.50f;
    L.sliderNy[2] = 0.62f;
    return L;
}

// reinicio del proceso (solo Windows)

#ifdef _WIN32
static bool fileExistsA(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void stripFileNameA(char* path)
{
    char* lastSlash = nullptr;
    for (char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/')
            lastSlash = p;
    if (lastSlash)
        *lastSlash = '\0';
}

static bool pathHasAssets(const char* dir)
{
    char probe[MAX_PATH];
    snprintf(probe, sizeof(probe), "%s\\shaders\\backrooms.fs", dir);
    if (fileExistsA(probe))
        return true;
    snprintf(probe, sizeof(probe), "%s/shaders/backrooms.fs", dir);
    return fileExistsA(probe);
}

static void copyIfNewerA(const char* src, const char* dst)
{
    if (!fileExistsA(src))
        return;
    WIN32_FILE_ATTRIBUTE_DATA srcInfo{}, dstInfo{};
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &srcInfo))
        return;
    if (GetFileAttributesExA(dst, GetFileExInfoStandard, &dstInfo))
    {
        if (CompareFileTime(&srcInfo.ftLastWriteTime, &dstInfo.ftLastWriteTime) <= 0)
            return;
    }
    char dir[MAX_PATH];
#if defined(_MSC_VER)
    strncpy_s(dir, sizeof(dir), dst, _TRUNCATE);
#else
    std::strncpy(dir, dst, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
#endif
    stripFileNameA(dir);
    CreateDirectoryA(dir, NULL);
    if (CopyFileA(src, dst, FALSE))
        std::cout << "[UI] REINICIAR sync: " << src << " -> " << dst << "\n";
}

static void syncShadersForRestart(const char* assetRoot)
{
    static const char* names[] = {
        "backrooms.fs", "backrooms.vs",
        "basico.fs", "basico.vs",
        "splash.fs", "splash.vs",
    };
    char src[MAX_PATH];
    char dst[MAX_PATH];
    for (const char* name : names)
    {
        snprintf(src, sizeof(src), "%s\\shaders\\%s", assetRoot, name);
        if (!fileExistsA(src))
            snprintf(src, sizeof(src), "%s/shaders/%s", assetRoot, name);
        if (!fileExistsA(src))
            continue;

        snprintf(dst, sizeof(dst), "%s\\x64\\Debug\\shaders\\%s", assetRoot, name);
        copyIfNewerA(src, dst);

        char cwd[MAX_PATH];
        if (GetCurrentDirectoryA(MAX_PATH, cwd) > 0)
        {
            if (_stricmp(cwd, assetRoot) != 0)
            {
                snprintf(dst, sizeof(dst), "%s\\shaders\\%s", cwd, name);
                copyIfNewerA(src, dst);
            }
        }
    }
}

static bool findAssetRootA(char* out, size_t outSize, const char* exePath)
{
    char cwd[MAX_PATH];
    if (GetCurrentDirectoryA(MAX_PATH, cwd) > 0 && pathHasAssets(cwd))
    {
#if defined(_MSC_VER)
        strncpy_s(out, outSize, cwd, _TRUNCATE);
#else
        std::strncpy(out, cwd, outSize - 1);
        out[outSize - 1] = '\0';
#endif
        return true;
    }

    char dir[MAX_PATH];
#if defined(_MSC_VER)
    strncpy_s(dir, sizeof(dir), exePath, _TRUNCATE);
#else
    std::strncpy(dir, exePath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
#endif
    stripFileNameA(dir);

    for (int i = 0; i < 4; ++i)
    {
        if (pathHasAssets(dir))
        {
#if defined(_MSC_VER)
            strncpy_s(out, outSize, dir, _TRUNCATE);
#else
            std::strncpy(out, dir, outSize - 1);
            out[outSize - 1] = '\0';
#endif
            return true;
        }
        stripFileNameA(dir);
        if (dir[0] == '\0')
            break;
    }
    return false;
}
#endif // _WIN32

void restartApplicationFast()
{
    std::cout << "[UI] REINICIAR...\n";
    AudioBgm_Shutdown();

#ifdef _WIN32
    char exePath[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        std::cerr << "[UI] REINICIAR fallo: no se pudo obtener la ruta del ejecutable.\n";
        return;
    }

    char assetRoot[MAX_PATH] = {};
    if (!findAssetRootA(assetRoot, sizeof(assetRoot), exePath))
    {
        GetCurrentDirectoryA(MAX_PATH, assetRoot);
        std::cout << "[UI] REINICIAR: asset root por defecto = " << assetRoot << "\n";
    }
    else
    {
        std::cout << "[UI] REINICIAR: asset root = " << assetRoot << "\n";
    }

    syncShadersForRestart(assetRoot);
    SetCurrentDirectoryA(assetRoot);

    glfwTerminate();

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    char cmdLine[MAX_PATH + 4];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\"", exePath);

    BOOL ok = CreateProcessA(
        exePath,
        cmdLine,
        NULL,
        NULL,
        FALSE,
        CREATE_NEW_PROCESS_GROUP,
        NULL,
        assetRoot,
        &si,
        &pi);

    if (!ok)
    {
        std::cerr << "[UI] REINICIAR fallo CreateProcess (err=" << GetLastError() << ")\n";
        std::exit(1);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
#else
    std::cerr << "[UI] REINICIAR no implementado en esta plataforma; cerrando.\n";
    std::exit(0);
#endif
}
