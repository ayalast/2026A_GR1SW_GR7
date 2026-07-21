#pragma once

// Texto 2D procedural (GDI a textura GL) para prompts y cinematicas Backrooms.
// Estilo: monoespaciado, ambar/sucio, parpadeo, typewriter.

struct TextBake
{
    unsigned int tex = 0;
    int width = 0;
    int height = 0;
};

// colorRgb 0..1, bg transparente si bgAlpha 0
// fontFace: opcional, p.ej. "Times New Roman" (default Consolas)
// fixedCanvasW/H: si >0 fuerza el tamano del lienzo (px) en vez de ajustarlo al
//   texto. Sirve para revelar texto (typewriter) manteniendo layout constante:
// el prefijo se dibuja arriba izquierda en un lienzo del tamano del texto final.
TextBake Text_Bake(const char* utf8, int pixelHeight, float r, float g, float b,
                   float bgR = 0.f, float bgG = 0.f, float bgB = 0.f, float bgA = 0.f,
                   int maxWidthPx = 0, const wchar_t* fontFace = nullptr,
                   int fixedCanvasW = 0, int fixedCanvasH = 0);

void Text_Free(TextBake& t);

// Dibuja textura centrada en NDC (cx,cy). halfH es mitad de altura en NDC.
// aspect es width/height del framebuffer.
void Text_DrawCentered(unsigned int tex, int texW, int texH,
                       float cx, float cy, float halfH, float aspect,
                       float alpha, unsigned int splashVAO, unsigned int splashVBO,
                       class Shader* splashShader);

// Rect NDC solido con textura 1x1 (o cualquier tex) , helper de overlay
void Text_DrawNdcQuad(float x0, float y0, float x1, float y1,
                      unsigned int tex, float alpha,
                      unsigned int splashVAO, unsigned int splashVBO,
                      class Shader* splashShader);
