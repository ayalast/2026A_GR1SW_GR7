#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform float uDim;
uniform float uAlpha;
void main()
{
    vec4 c = texture(uTex, vUV);
    c.rgb *= (1.0 - uDim * 0.25);
    float a = uAlpha;
    // Si no se setea uAlpha (0 por defecto en GL), tratamos como opaco para menús/splash
    // Lo seteamos siempre desde C++ a 1.0 para UI y a fade para overlay.
    FragColor = vec4(c.rgb, c.a * a);
}
