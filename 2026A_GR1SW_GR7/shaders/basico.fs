#version 330 core
out vec4 FragColor;

// Cambiamos a vec4 para poder recibir un valor de transparencia (Alpha)
uniform vec4 cubeColor; 

void main(){
    FragColor = cubeColor; // Pasamos el color directamente con su transparencia
}