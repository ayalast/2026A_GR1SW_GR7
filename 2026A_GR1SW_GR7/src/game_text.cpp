#include "game_text.h"

#include <shader.h>

#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

static std::wstring utf8ToWide(const char* utf8)
{
    if (!utf8 || !utf8[0])
        return L"";
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0)
        return L"";
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    return out;
#else
    // fallback basico
    std::wstring out;
    for (const char* p = utf8; *p; ++p)
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    return out;
#endif
}

TextBake Text_Bake(const char* utf8, int pixelHeight, float r, float g, float b,
                   float bgR, float bgG, float bgB, float bgA, int maxWidthPx,
                   const wchar_t* fontFace, int fixedCanvasW, int fixedCanvasH)
{
    TextBake out{};
    if (!utf8 || !utf8[0] || pixelHeight < 8)
        return out;

#ifdef _WIN32
    std::wstring w = utf8ToWide(utf8);
    if (w.empty())
        return out;

    HDC screen = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(screen);
    if (!hdc)
    {
        ReleaseDC(nullptr, screen);
        return out;
    }

    int h = std::max(12, pixelHeight);
    const wchar_t* face = (fontFace && fontFace[0]) ? fontFace : L"Consolas";
    const bool serif = (fontFace && fontFace[0] &&
        (wcsstr(fontFace, L"Times") || wcsstr(fontFace, L"Georgia") || wcsstr(fontFace, L"Garamond")));
    DWORD pitch = serif ? (VARIABLE_PITCH | FF_ROMAN) : (FIXED_PITCH | FF_MODERN);
    int weight = serif ? FW_BOLD : FW_SEMIBOLD;
    HFONT font = CreateFontW(
        -h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, pitch, face);
    if (!font)
        font = CreateFontW(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, pitch, L"Times New Roman");
    if (!font)
        font = CreateFontW(-h, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    HGDIOBJ oldFont = SelectObject(hdc, font);

    // Multilinea: \n y word wrap. CALCRECT necesita ancho base y alto grande.
    const bool multi = (w.find(L'\n') != std::wstring::npos) || (maxWidthPx > 0);
    int wrapW = maxWidthPx > 0 ? maxWidthPx : (multi ? 900 : 8);
    RECT rc{ 0, 0, wrapW, 4096 };
    UINT flags = DT_CALCRECT | DT_LEFT | DT_NOPREFIX | DT_TOP;
    if (multi)
        flags |= DT_WORDBREAK;
    DrawTextW(hdc, w.c_str(), -1, &rc, flags);
    int tw = rc.right - rc.left + 24;
    int th = rc.bottom - rc.top + 20;
    if (maxWidthPx > 0)
        tw = std::min(tw, maxWidthPx + 24);
    tw = std::max(tw, 8);
    th = std::max(th, h + 8);
    // Lienzo fijo: el texto (prefijo) se dibuja arriba izquierda dentro de un
    // canvas del tamano del mensaje completo a layout constante al revelar.
    if (fixedCanvasW > 0)
        tw = fixedCanvasW;
    if (fixedCanvasH > 0)
        th = fixedCanvasH;
    // potencias de 2 no obligatorias en GL3

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -th; // top down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits)
    {
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        DeleteDC(hdc);
        ReleaseDC(nullptr, screen);
        return out;
    }

    HGDIOBJ oldBmp = SelectObject(hdc, bmp);

    const BYTE bgA8 = static_cast<BYTE>(std::clamp(bgA, 0.f, 1.f) * 255.f);
    const BYTE bgR8 = static_cast<BYTE>(std::clamp(bgR, 0.f, 1.f) * 255.f);
    const BYTE bgG8 = static_cast<BYTE>(std::clamp(bgG, 0.f, 1.f) * 255.f);
    const BYTE bgB8 = static_cast<BYTE>(std::clamp(bgB, 0.f, 1.f) * 255.f);
    // BGRA fill
    auto* px = static_cast<unsigned char*>(bits);
    for (int i = 0; i < tw * th; ++i)
    {
        px[i * 4 + 0] = bgB8;
        px[i * 4 + 1] = bgG8;
        px[i * 4 + 2] = bgR8;
        px[i * 4 + 3] = bgA8;
    }

    SetBkMode(hdc, TRANSPARENT);
    const COLORREF color = RGB(
        static_cast<int>(std::clamp(r, 0.f, 1.f) * 255.f),
        static_cast<int>(std::clamp(g, 0.f, 1.f) * 255.f),
        static_cast<int>(std::clamp(b, 0.f, 1.f) * 255.f));
    SetTextColor(hdc, color);

    RECT drawRc{ 12, 8, tw - 12, th - 8 };
    UINT drawFlags = DT_LEFT | DT_NOPREFIX | DT_TOP;
    if (multi)
        drawFlags |= DT_WORDBREAK;
    else
        drawFlags |= DT_SINGLELINE | DT_VCENTER;
    DrawTextW(hdc, w.c_str(), -1, &drawRc, drawFlags);

    // Premultiplicar / forzar alpha en pixeles de texto (GDI no escribe alpha bien)
    for (int i = 0; i < tw * th; ++i)
    {
        unsigned char bb = px[i * 4 + 0];
        unsigned char gg = px[i * 4 + 1];
        unsigned char rr = px[i * 4 + 2];
        // si se parece al fondo, alpha bajo; si tiene tinta, alpha alto
        int dr = std::abs((int)rr - (int)bgR8);
        int dg = std::abs((int)gg - (int)bgG8);
        int db = std::abs((int)bb - (int)bgB8);
        int diff = dr + dg + db;
        if (diff > 18)
            px[i * 4 + 3] = 255;
        else if (bgA8 == 0)
            px[i * 4 + 3] = 0;
    }

    glGenTextures(1, &out.tex);
    glBindTexture(GL_TEXTURE_2D, out.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_BGRA, GL_UNSIGNED_BYTE, bits);
    out.width = tw;
    out.height = th;

    SelectObject(hdc, oldBmp);
    SelectObject(hdc, oldFont);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(hdc);
    ReleaseDC(nullptr, screen);
#else
    (void)r; (void)g; (void)b; (void)bgR; (void)bgG; (void)bgB; (void)bgA; (void)maxWidthPx;
    std::cout << "[Text] Bake solo soportado en Windows\n";
#endif
    return out;
}

void Text_Free(TextBake& t)
{
    if (t.tex)
    {
        glDeleteTextures(1, &t.tex);
        t.tex = 0;
    }
    t.width = t.height = 0;
}

void Text_DrawNdcQuad(float x0, float y0, float x1, float y1,
                      unsigned int tex, float alpha,
                      unsigned int splashVAO, unsigned int splashVBO,
                      Shader* splashShader)
{
    if (!splashShader || !splashVAO || !tex)
        return;
    // GDI top down DIB: primera fila de memoria es parte SUPERIOR del texto.
    // OpenGL asume primera fila es v 0 (abajo). Invertimos V para texto derecho.
    float q[] = {
        x0, y0, 0.f, 1.f,
        x1, y0, 1.f, 1.f,
        x1, y1, 1.f, 0.f,
        x0, y0, 0.f, 1.f,
        x1, y1, 1.f, 0.f,
        x0, y1, 0.f, 0.f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, splashVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(q), q);
    splashShader->use();
    splashShader->setFloat("uDim", 0.0f);
    splashShader->setFloat("uAlpha", alpha);
    splashShader->setInt("uTex", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(splashVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    float full[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
         1.f,  1.f, 1.f, 1.f,
        -1.f, -1.f, 0.f, 0.f,
         1.f,  1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 1.f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, splashVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(full), full);
}

void Text_DrawCentered(unsigned int tex, int texW, int texH,
                       float cx, float cy, float halfH, float aspect,
                       float alpha, unsigned int splashVAO, unsigned int splashVBO,
                       Shader* splashShader)
{
    if (!tex || texW <= 0 || texH <= 0 || halfH <= 0.f)
        return;
    // halfH en NDC (Y); compensar aspect para pixeles cuadrados
    const float hw = halfH * (float(texW) / float(std::max(1, texH))) / std::max(aspect, 0.01f);
    Text_DrawNdcQuad(cx - hw, cy - halfH, cx + hw, cy + halfH,
                     tex, alpha, splashVAO, splashVBO, splashShader);
}
