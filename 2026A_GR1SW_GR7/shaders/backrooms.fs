#version 330 core
out vec4 FragColor;

struct DirLight {
    vec3 direction;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

struct PointLight {
    vec3 position;
    float constant;
    float linear;
    float quadratic;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

struct SpotLight {
    vec3 position;
    vec3 direction;
    float cutOff;
    float outerCutOff;
    float constant;
    float linear;
    float quadratic;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

#define NR_POINT_LIGHTS 120

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;

uniform vec3 viewPos;
uniform DirLight dirLight;
uniform PointLight pointLights[NR_POINT_LIGHTS];
uniform int numActivePointLights;
uniform SpotLight spotLight;

uniform sampler2D texture_diffuse1;
uniform sampler2D texture_specular1;
uniform float shininess;
uniform bool use_specular_map;

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir, vec3 albedo);
vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 albedo, bool isCeiling);
vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 albedo);

void main()
{
    vec3 albedo = vec3(texture(texture_diffuse1, TexCoords));
    vec3 norm = normalize(Normal);

    // Techo: caras traseras o normales mirando hacia abajo
    bool isCeiling = (!gl_FrontFacing) || (norm.y < -0.45);
    if (!gl_FrontFacing)
        norm = -norm;
    if (isCeiling && norm.y > 0.0)
        norm = -norm;

    // Suelo: normal hacia arriba
    bool isFloor = (!isCeiling) && (norm.y > 0.55);

    vec3 viewDir = normalize(viewPos - FragPos);

    vec3 result = CalcDirLight(dirLight, norm, viewDir, albedo);

    int lightsToCount = numActivePointLights;
    if (lightsToCount > NR_POINT_LIGHTS)
        lightsToCount = NR_POINT_LIGHTS;

    for (int i = 0; i < lightsToCount; i++)
        result += CalcPointLight(pointLights[i], norm, FragPos, viewDir, albedo, isCeiling);

    result += CalcSpotLight(spotLight, norm, FragPos, viewDir, albedo);

    // --- Paneles de techo: emision solo si hay luz real; apagados en dark ---
    // Los cuadrados blancos son albedo del mesh; sin esto se ven "encendidos" en zonas oscuras.
    if (isCeiling)
    {
        float litAmount = length(result);
        // Mas estricto: paneles no brillan con luz residual debil
        float litMask = smoothstep(0.04, 0.18, litAmount);

        // Texels muy claros = panel fluorescente (vs rejilla gris del techo)
        float luma = dot(albedo, vec3(0.299, 0.587, 0.114));
        float panelMask = smoothstep(0.55, 0.82, luma);

        // Boost + auto-emision SOLO en paneles de zonas lit
        result *= mix(1.0, 1.55, litMask * panelMask);
        result += albedo * 0.38 * litMask * panelMask;

        // En dark: paneles blancos se apagan (gris humo), rejilla un poco menos
        float darkPanel = (1.0 - litMask);
        result *= mix(1.0, mix(0.55, 0.12, panelMask), darkPanel);
    }

    // Relleno muy suave en paredes/suelo para evitar triangulos negros duros
    // (no elimina el contraste lit/dark; solo evita negro de video)
    if (!isCeiling)
    {
        float fill = isFloor ? 0.018 : 0.028;
        result += albedo * fill;
    }

    // Niebla a distancia
    float distToCamera = length(viewPos - FragPos);
    float fogStart = 70.0;
    float fogEnd = 160.0;
    float fogFactor = smoothstep(fogStart, fogEnd, distToCamera);
    result = mix(result, vec3(0.0), fogFactor);

    // Evitar saturacion
    result = min(result, vec3(1.05));
    result = result / (result + vec3(0.35)) * 1.15;
    result = clamp(result, 0.0, 1.0);

    FragColor = vec4(result, 1.0);
}

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir, vec3 albedo)
{
    vec3 lightDir = normalize(-light.direction);
    // Half-Lambert: suaviza bordes duros en paredes (menos triangulos negros)
    float ndotl = dot(normal, lightDir);
    float diff = ndotl * 0.5 + 0.5;
    diff = diff * diff;

    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 ambient = light.ambient * albedo;
    vec3 diffuse = light.diffuse * diff * albedo;
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue;
    return (ambient + diffuse + specular);
}

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 albedo, bool isCeiling)
{
    vec3 lightDir = normalize(light.position - fragPos);
    float ndotl = dot(normal, lightDir);

    // Paredes: half-Lambert para penumbra natural
    float diff = ndotl * 0.5 + 0.5;
    diff = diff * diff;

    // Techos: wrap moderado (no tan agresivo como antes: menos techo blanco en dark vecino)
    if (isCeiling || normal.y < -0.35)
    {
        float wrap = max(ndotl, 0.0) * 0.40 + 0.60;
        float ceilDiff = wrap * 0.55;
        if (light.position.y <= fragPos.y + 0.75)
            ceilDiff = max(ceilDiff, 0.48);
        diff = max(diff * 0.35, ceilDiff);
    }

    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    float distance = length(light.position - fragPos);
    // Atenuacion un poco mas suave que antes (bordes de charco menos cortantes)
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));

    vec3 ambient = light.ambient * albedo;
    vec3 diffuse = light.diffuse * diff * albedo;
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue;
    ambient *= attenuation;
    diffuse *= attenuation;
    specular *= attenuation;
    return (ambient + diffuse + specular);
}

vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 albedo)
{
    // Direccion del fragmento visto DESDE la linterna (eje del cono)
    vec3 toFrag = fragPos - light.position;
    float distance = length(toFrag);
    if (distance < 1e-4)
        return vec3(0.0);

    vec3 spotDir = normalize(light.direction);
    vec3 rayDir = toFrag / distance;

    // Angulo del cono: esto debe dominar el "circulo" (sigue la mirada)
    float theta = dot(rayDir, spotDir);
    float epsilon = light.cutOff - light.outerCutOff;
    float intensity = clamp((theta - light.outerCutOff) / max(epsilon, 1e-4), 0.0, 1.0);
    // Curva mas marcada en el centro para que el circulo interior se note al mirar
    intensity = intensity * intensity;

    // Si esta fuera del cono exterior, cero
    if (intensity <= 0.0)
        return vec3(0.0);

    // Iluminacion de superficie (desde frag hacia la luz)
    vec3 lightDir = -rayDir;
    float ndotl = max(dot(normal, lightDir), 0.0);
    // Un poco de wrap para no dejar agujeros negros en paredes de frente
    float diff = ndotl * 0.75 + 0.25;

    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);

    // Atenuacion por distancia: mas suave para que el angulo mande sobre el "punto mas cercano a la camara"
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
    // Refuerzo angular extra: el hotspot sigue el centro del cono, no el pie de la normal a la pared
    float core = smoothstep(light.outerCutOff, light.cutOff, theta);
    core = core * core;

    vec3 ambient = light.ambient * albedo;
    vec3 diffuse = light.diffuse * diff * albedo;
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue;

    float gain = attenuation * intensity * (0.55 + 0.45 * core);
    ambient *= gain;
    diffuse *= gain;
    specular *= gain;
    return (ambient + diffuse + specular);
}
