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
// true = paredes del mapa; false = props
uniform bool applyLevelWallpaper;
// true = blackout
uniform bool lightsBlackout;
// 1.0 = modo admiracion
uniform float admiracionAmount;
// 0..1 parpadeo de las luces en admiracion
uniform float admiracionFlicker;

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir, vec3 albedo);
vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 albedo, bool isCeiling);
vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 albedo);

void main()
{
    // Fuerza del patron de pared
    const float WALL_PATTERN_STRENGTH = 0.58;

    vec3 albedo = vec3(texture(texture_diffuse1, TexCoords));
    vec3 norm = normalize(Normal);

    // Invertir normales de back-face
    if (!gl_FrontFacing)
        norm = -norm;

    bool isCeiling = (norm.y < -0.45);
    bool isFloor   = (norm.y > 0.55);
    bool isWall    = (!isCeiling) && (!isFloor);
    bool levelWall = applyLevelWallpaper && isWall;

    if (levelWall)
    {
        float aL = max(dot(albedo, vec3(0.299, 0.587, 0.114)), 1e-4);
        vec3 chroma = albedo / aL;
        float softL = mix(0.52, aL, WALL_PATTERN_STRENGTH);
        albedo = chroma * softL;
    }

    // Paneles de techo: gris si estan apagados
    float ceilingPanelMask = 0.0;
    float admPanelOff = 0.0; // 0 = encendido, 1 = apagado total del objeto lampara
    if (isCeiling)
    {
        float cLuma = dot(albedo, vec3(0.299, 0.587, 0.114));
        ceilingPanelMask = smoothstep(0.50, 0.78, cLuma);
        if (lightsBlackout)
        {
            albedo = mix(albedo, vec3(0.10, 0.095, 0.085), ceilingPanelMask);
            admPanelOff = 1.0;
        }
        else if (admiracionAmount > 0.5)
        {
            float fl = clamp(admiracionFlicker, 0.0, 1.0);
            admPanelOff = 1.0 - fl;
            albedo = mix(albedo, vec3(0.10, 0.095, 0.085), ceilingPanelMask * admPanelOff);
        }
    }

    vec3 viewDir = normalize(viewPos - FragPos);

    // Albedo de shade (paredes del mapa en blanco para la luz)
    vec3 shadeAlbedo = levelWall ? vec3(1.0) : albedo;
    if (isCeiling && (lightsBlackout || admPanelOff > 0.5))
        shadeAlbedo = albedo;
    else if (isCeiling && admiracionAmount > 0.5 && admPanelOff > 0.01)
        shadeAlbedo = mix(shadeAlbedo, albedo, ceilingPanelMask * admPanelOff);

    vec3 result = CalcDirLight(dirLight, norm, viewDir, shadeAlbedo);

    int lightsToCount = numActivePointLights;
    if (lightsToCount > NR_POINT_LIGHTS)
        lightsToCount = NR_POINT_LIGHTS;

    // Point lights + soft-knee (evita pared final blanca)
    vec3 pointAccum = vec3(0.0);
    for (int i = 0; i < lightsToCount; i++)
        pointAccum += CalcPointLight(pointLights[i], norm, FragPos, viewDir, shadeAlbedo, isCeiling);

    {
        float pLuma = max(dot(pointAccum, vec3(0.299, 0.587, 0.114)), 0.0);
        float knee = 0.72;
        float compress = (pLuma <= knee)
            ? pLuma
            : knee + (pLuma - knee) / (1.0 + (pLuma - knee) * 2.4);
        compress = compress / (1.0 + compress * 0.55);
        float scale = (pLuma > 1e-5) ? (compress / pLuma) : 1.0;
        if (!isCeiling)
            scale *= 0.92;
        pointAccum *= scale;
    }
    result += pointAccum;

    result += CalcSpotLight(spotLight, norm, FragPos, viewDir, shadeAlbedo);

    // Paredes del nivel: aplicar iluminacion al albedo
    bool admLit = (admiracionAmount > 0.5) && (admiracionFlicker > 0.05) && (!lightsBlackout);
    if (levelWall)
    {
        if (admLit)
        {
            float aL = max(dot(albedo, vec3(0.299, 0.587, 0.114)), 1e-4);
            vec3 redWallAlb = vec3(aL * 1.18, aL * 0.20, aL * 0.12);
            vec3 wallAlb = mix(albedo, redWallAlb, 0.88);
            result = result * wallAlb;
        }
        else
        {
            float lightAmt = max(dot(result, vec3(0.299, 0.587, 0.114)), 0.0);
            result = albedo * lightAmt;
        }
    }

    float distToCamera = length(viewPos - FragPos);

    // Paneles de techo
    if (isCeiling)
    {
        float panelMask = ceilingPanelMask;
        bool panelFullyOff = lightsBlackout || (admiracionAmount > 0.5 && admiracionFlicker < 0.05);
        if (panelFullyOff)
        {
            float ceilExtinct = smoothstep(85.0, 165.0, distToCamera);
            result *= (1.0 - ceilExtinct * 0.88);
            float rL = max(dot(result, vec3(0.299, 0.587, 0.114)), 0.0);
            float cap = mix(1.0, 0.22, panelMask);
            if (rL > 1e-5)
                result *= min(1.0, cap / rL);
        }
        else
        {
            float litAmount = length(result);
            float litMask = smoothstep(0.08, 0.28, litAmount);

            float nearVis = 1.0 - smoothstep(70.0, 150.0, distToCamera);
            nearVis = nearVis * nearVis * (3.0 - 2.0 * nearVis);

            float glow = litMask * nearVis;
            float glowMul = (admiracionAmount > 0.5) ? clamp(admiracionFlicker, 0.0, 1.0) : 1.0;
            glow *= glowMul;

            result *= mix(1.0, 1.45, glow * panelMask);
            result += albedo * 0.32 * glow * panelMask;
            if (admiracionAmount > 0.5)
                result += vec3(0.55, 0.06, 0.04) * glow * panelMask;

            float darkPanel = 1.0 - glow;
            result *= mix(1.0, mix(0.40, 0.035, panelMask), darkPanel);

            if (admiracionAmount > 0.5 && admPanelOff > 0.01)
            {
                float rL = max(dot(result, vec3(0.299, 0.587, 0.114)), 0.0);
                float cap = mix(1.0, 0.22, panelMask);
                float dimScale = mix(1.0, min(1.0, cap / max(rL, 1e-5)), admPanelOff * panelMask);
                result *= dimScale;
            }

            float ceilExtinct = smoothstep(85.0, 165.0, distToCamera);
            result *= (1.0 - ceilExtinct * 0.88);
        }
    }

    // Fill ambiente solo con luces encendidas
    bool noAmbientFill = lightsBlackout
        || (admiracionAmount > 0.5 && admiracionFlicker < 0.05);
    if (!isCeiling && !noAmbientFill)
    {
        float fill = isFloor ? 0.014 : 0.020;
        result += albedo * fill;
        if (admiracionAmount > 0.5)
            result += vec3(0.025, 0.003, 0.002) * albedo * clamp(admiracionFlicker, 0.0, 1.0);
    }

    // Reaplicar el color del wallpaper en paredes
    if (levelWall && !admLit)
    {
        float aL = max(dot(albedo, vec3(0.299, 0.587, 0.114)), 1e-4);
        float rL = max(dot(result, vec3(0.299, 0.587, 0.114)), 0.0);
        result = albedo * (rL / aL);
    }

    // Tinte rojo del modo admiracion
    if (admiracionAmount > 0.5)
    {
        float fl = clamp(admiracionFlicker, 0.0, 1.0);
        float L = max(dot(result, vec3(0.299, 0.587, 0.114)), 0.0);
        vec3 redGrade = vec3(L * 1.22, L * 0.15, L * 0.10);
        float gradeMix = 0.82 * max(fl, 0.10);
        if (fl < 0.05)
            gradeMix = 0.0;
        if (isWall)
            gradeMix = min(gradeMix + 0.10 * fl, 0.92);
        result = mix(result, redGrade, gradeMix);
        result += vec3(0.05, 0.005, 0.003) * admiracionAmount * fl;
    }

    // Niebla a distancia
    float fogStart = 75.0;
    float fogEnd = 165.0;
    float fogFactor = smoothstep(fogStart, fogEnd, distToCamera);
    vec3 fogColor = vec3(0.0);
    if (admiracionAmount > 0.5)
        fogColor = (admiracionFlicker < 0.05)
            ? vec3(0.0)
            : vec3(0.04, 0.005, 0.004);
    result = mix(result, fogColor, fogFactor);

    // Tone map
    float lumaIn = max(dot(result, vec3(0.299, 0.587, 0.114)), 1e-5);
    float mapped = (lumaIn / (lumaIn + 0.62)) * 1.22;
    mapped = min(mapped, 0.92 + mapped * 0.06);
    result = result * (mapped / lumaIn);
    result = clamp(result, 0.0, 1.0);

    FragColor = vec4(result, 1.0);
}

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir, vec3 albedo)
{
    vec3 lightDir = normalize(-light.direction);
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
    vec3 toLight = light.position - fragPos;
    float distance = length(toLight);
    vec3 lightDir = toLight / max(distance, 1e-4);
    float ndotl = dot(normal, lightDir);

    // Misma clasificacion que main() (sin gl_FrontFacing)
    bool isFloorSurf = (!isCeiling) && (normal.y > 0.55);
    bool isWall = (!isCeiling) && (!isFloorSurf);

    float diff;
    float shape = 1.0;

    if (isCeiling || normal.y < -0.35)
    {
        float wrap = max(ndotl, 0.0) * 0.40 + 0.60;
        float ceilDiff = wrap * 0.55;
        if (light.position.y <= fragPos.y + 0.75)
            ceilDiff = max(ceilDiff, 0.48);
        float hl = ndotl * 0.5 + 0.5;
        hl = hl * hl;
        diff = max(hl * 0.35, ceilDiff);
    }
    else if (isWall)
    {
        // Manchas de luz redondas en la pared
        float planeOff = abs(dot(toLight, normal));
        vec3 onPlane = toLight - normal * dot(toLight, normal);
        float radial = length(onPlane);

        float sigma = 5.2 + planeOff * 0.42;
        float circle = exp(-(radial * radial) / (2.0 * sigma * sigma));
        float outer = 1.0 - smoothstep(sigma * 0.9, sigma * 2.4, radial);

        shape = max(circle, outer * 0.40);
        shape = clamp(shape, 0.0, 1.0);

        float facing = 0.55 + 0.45 * max(ndotl, 0.0);
        diff = facing;
    }
    else if (isFloorSurf)
    {
        float hl = ndotl * 0.5 + 0.5;
        diff = hl * hl;
        float horiz = length(vec2(toLight.x, toLight.z));
        float floorCircle = exp(-(horiz * horiz) / (2.0 * 7.5 * 7.5));
        shape = mix(0.55, 1.0, floorCircle);
    }
    else
    {
        float hl = ndotl * 0.5 + 0.5;
        diff = hl * hl;
    }

    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);

    float attenPhys = 1.0 / (light.constant + light.linear * distance
                           + light.quadratic * (distance * distance));
    float attenuation = isWall ? pow(max(attenPhys, 1e-6), 0.82) : attenPhys;

    float ambShape = isWall ? mix(0.35, 0.85, shape) : 1.0;
    float difShape = isWall ? shape : shape;

    vec3 ambient = light.ambient * albedo * attenuation * ambShape;
    vec3 diffuse = light.diffuse * diff * albedo * attenuation * difShape;
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    float specMul = isWall ? 0.06 : 1.0;
    vec3 specular = light.specular * spec * specularValue * attenuation * difShape * specMul;
    return (ambient + diffuse + specular);
}

vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 albedo)
{
    vec3 toFrag = fragPos - light.position;
    float distance = length(toFrag);
    if (distance < 1e-4)
        return vec3(0.0);

    vec3 spotDir = normalize(light.direction);
    vec3 rayDir = toFrag / distance;

    float theta = dot(rayDir, spotDir);
    float epsilon = light.cutOff - light.outerCutOff;
    float intensity = clamp((theta - light.outerCutOff) / max(epsilon, 1e-4), 0.0, 1.0);
    intensity = intensity * intensity;

    if (intensity <= 0.0)
        return vec3(0.0);

    vec3 lightDir = -rayDir;
    float ndotl = max(dot(normal, lightDir), 0.0);
    // En blackout la linterna ilumina las paredes de forma mas pareja
    float diff = lightsBlackout
        ? mix(0.58, 0.66, ndotl)
        : (ndotl * 0.75 + 0.25);
    float spotSpecMul = lightsBlackout ? 0.08 : 1.0;

    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);

    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
    float core = smoothstep(light.outerCutOff, light.cutOff, theta);
    core = core * core;

    vec3 ambient = light.ambient * albedo;
    vec3 diffuse = light.diffuse * diff * albedo;
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue * spotSpecMul;

    float gain = attenuation * intensity * (0.55 + 0.45 * core);
    ambient *= gain;
    diffuse *= gain;
    specular *= gain;
    return (ambient + diffuse + specular);
}
