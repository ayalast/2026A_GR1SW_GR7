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

void main(){ 
    vec3 norm = normalize(Normal);
    
    // Identificar el techo
    bool isCeiling = !gl_FrontFacing;
    
    if (isCeiling) {
        norm = -norm; // Voltear la normal del techo
    }
    
    vec3 viewDir = normalize(viewPos - FragPos);

    // Calcular luz global
    vec3 result = CalcDirLight(dirLight, norm, viewDir);
    
    // Extra de brillo solo para el techo
    if (isCeiling) {
        vec3 ceilingBoost = vec3(0.35) * vec3(texture(texture_diffuse1, TexCoords));
        result += ceilingBoost;
    }

    int lightsToCount = numActivePointLights;
    if (lightsToCount > NR_POINT_LIGHTS) {
        lightsToCount = NR_POINT_LIGHTS;
    }
    
    for(int i = 0; i < lightsToCount; i++) {
        result += CalcPointLight(pointLights[i], norm, FragPos, viewDir);    
    }
    
    result += CalcSpotLight(spotLight, norm, FragPos, viewDir);    
    

    float distToCamera = length(viewPos - FragPos);
    
    float fogStart = 10.0; 
    float fogEnd = 40.0;   
    
    // clamp limita el valor entre 0.0 y 1.0
    float fogFactor = clamp((distToCamera - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    
    // El color con el que se va a mezclar (Negro profundo)
    vec3 darknessColor = vec3(0.0f, 0.0f, 0.0f); 
    
    // Mezclamos el resultado final con la oscuridad basándonos en la distancia
    result = mix(result, darknessColor, fogFactor);
    // ===================================================
    
    FragColor = vec4(result, 1.0);  
}

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir)
{
    vec3 lightDir = normalize(-light.direction);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 ambient = light.ambient * vec3(texture(texture_diffuse1, TexCoords));
    vec3 diffuse = light.diffuse * diff * vec3(texture(texture_diffuse1, TexCoords));
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue;
    return (ambient + diffuse + specular);
}

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir)
{
    vec3 lightDir = normalize(light.position - fragPos);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    float distance = length(light.position - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));    
    vec3 ambient = light.ambient * vec3(texture(texture_diffuse1, TexCoords));
    vec3 diffuse = light.diffuse * diff * vec3(texture(texture_diffuse1, TexCoords));
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
    vec3 ambient = light.ambient * vec3(texture(texture_diffuse1, TexCoords));
    vec3 diffuse = light.diffuse * diff * vec3(texture(texture_diffuse1, TexCoords));
    vec3 specularValue = use_specular_map ? vec3(texture(texture_specular1, TexCoords)) : vec3(0.1);
    vec3 specular = light.specular * spec * specularValue;
    ambient *= attenuation * intensity;
    diffuse *= attenuation * intensity;
    specular *= attenuation * intensity;
    return (ambient + diffuse + specular);
}