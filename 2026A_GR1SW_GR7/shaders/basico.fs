#version 330 core
out vec4 FragColor;

in vec3 FragPos;

// Color base de la sombra plana (alpha = opacidad max fuera del haz)
uniform vec4 cubeColor;

// Linterna: SOLO dentro del cono se anula la sombra (fuera queda intacta)
uniform bool flashlightOn;
uniform vec3 flashPos;
uniform vec3 flashDir;
uniform float flashCutOff;      // cos(inner), igual que spot del juego
uniform float flashOuterCutOff; // cos(outer)
uniform float flashRange;

void main()
{
    float alpha = cubeColor.a;

    if (flashlightOn)
    {
        vec3 toFrag = FragPos - flashPos;
        float dist = length(toFrag);

        if (dist > 1e-4 && dist < flashRange)
        {
            vec3 rayDir = toFrag / dist;
            vec3 fdir = normalize(flashDir);
            float theta = dot(rayDir, fdir);

            // Mismo cono que la linterna del mundo (no nearKill por distancia)
            float epsilon = max(flashCutOff - flashOuterCutOff, 1e-4);
            float intensity = clamp((theta - flashOuterCutOff) / epsilon, 0.0, 1.0);
            intensity = intensity * intensity;

            // Atenuacion alineada con spotLight del juego
            float atten = 1.0 / (1.0 + 0.018 * dist + 0.0045 * dist * dist);
            // Centro del haz: lit~1 (sin sombra). Fuera del cono: lit=0 (sombra llena).
            float lit = clamp(intensity * atten * 2.8, 0.0, 1.0);

            alpha *= (1.0 - lit);
        }
        // Fuera de flashRange: alpha sin tocar, sombra visible
    }

    if (alpha < 0.02)
        discard;

    FragColor = vec4(cubeColor.rgb, alpha);
}
