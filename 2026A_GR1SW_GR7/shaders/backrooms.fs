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

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir);
vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir);
vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir);

void main()
{
    vec3 albedo = vec3(texture(texture_diffuse1, TexCoords));
    vec3 norm = normalize(Normal);

    // Techo: caras traseras o normales mirando hacia abajo
    bool isCeiling = (!gl_FrontFacing) || (norm.y < -0.45);
    if (!gl_FrontFacing)
        norm = -norm;
    // Si la normal sigue apuntando hacia arriba en un techo, forzar hacia abajo
    if (isCeiling && norm.y > 0.0)
        norm = -norm;

    vec3 viewDir = normalize(viewPos - FragPos);

    vec3 result = CalcDirLight(dirLight, norm, viewDir);

    int lightsToCount = numActivePointLights;
    if (lightsToCount > NR_POINT_LIGHTS)
        lightsToCount = NR_POINT_LIGHTS;

    for (int i = 0; i < lightsToCount; i++)
        result += CalcPointLight(pointLights[i], norm, FragPos, viewDir);

    result += CalcSpotLight(spotLight, norm, FragPos, viewDir);

    // Boost del techo SOLO donde ya hay algo de luz (zonas lit), no en oscuridad total
    if (isCeiling)
    {
        float litAmount = length(result);
        float litMask = smoothstep(0.015, 0.12, litAmount);
        result *= mix(1.0, 1.95, litMask);
        // Auto-emision de paneles fluorescentes solo si la zona esta iluminada
        result += albedo * 0.42 * litMask;
    }

    // Niebla a distancia
    float distToCamera = length(viewPos - FragPos);
    float fogStart = 70.0;
    float fogEnd = 160.0;
    float fogFactor = smoothstep(fogStart, fogEnd, distToCamera);
    result = mix(result, vec3(0.0), fogFactor);

    // Evitar saturacion en zonas muy lit (brillante pero no quemado)
    result = min(result, vec3(1.05));
    // Compresion suave de highlights
    result = result / (result + vec3(0.35)) * 1.15;
    result = clamp(result, 0.0, 1.0);

    FragColor = vec4(result, 1.0);
}

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir)
{
    vec3 lightDir = normalize(-light.direction);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 albedo = vec3(texture(texture_diffuse1, TexCoords));
    vec3 ambient = light.ambient * albedo;
    vec3 diffuse = light.diffuse * diff * albedo;
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue;
    return (ambient + diffuse + specular);
}

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir)
{
    vec3 lightDir = normalize(light.position - fragPos);
    float diff = max(dot(normal, lightDir), 0.0);

    // Techos: relleno suave (los tubos casi en el plano no iluminan bien solo con Lambert)
    if (normal.y < -0.35)
    {
        float wrap = max(dot(normal, lightDir), 0.0) * 0.45 + 0.55;
        diff = max(diff, wrap * 0.70);
        // Si la luz esta por debajo del fragmento de techo, reforzar
        if (light.position.y <= fragPos.y + 0.75)
            diff = max(diff, 0.72);
    }

    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    float distance = length(light.position - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
    vec3 albedo = vec3(texture(texture_diffuse1, TexCoords));
    vec3 ambient = light.ambient * albedo;
    vec3 diffuse = light.diffuse * diff * albedo;
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue;
    ambient *= attenuation;
    diffuse *= attenuation;
    specular *= attenuation;
    return (ambient + diffuse + specular);
}

vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir)
{
    vec3 lightDir = normalize(light.position - fragPos);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    float distance = length(light.position - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
    float theta = dot(lightDir, normalize(-light.direction));
    float epsilon = light.cutOff - light.outerCutOff;
    float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);
    vec3 albedo = vec3(texture(texture_diffuse1, TexCoords));
    vec3 ambient = light.ambient * albedo;
    vec3 diffuse = light.diffuse * diff * albedo;
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue;
    ambient *= attenuation * intensity;
    diffuse *= attenuation * intensity;
    specular *= attenuation * intensity;
    return (ambient + diffuse + specular);
}
