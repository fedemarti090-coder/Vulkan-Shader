#version 460

/*
          -y
          |
          |
          |
-x--------C--------+x   C= (0,0)
          |
          |
          |
          +y

-1 < x < 1
-1 < y < 1
*/

//angoli del triangolo  (hardcoded)
const vec3 positions[3] = vec3[]
(
    vec3(  0.0, -0.5,  0.0),
    vec3( -0.5,  0.5,  0.0),
    vec3(  0.5,  0.5,  0.0)
);

//angoli del colore
const vec3 colors[3] = vec3[]
(
    vec3( 1.0, 0.0, 0.0),
    vec3( 0.0, 1.0, 0.0),
    vec3( 0.0, 0.0, 1.0)
);

layout (location = 0) out vec3 outColor; // output --> outColor [R, G, B]

void main(){

    //gl_vertexIndex arriva da Vulkan quale vertex stiamo processando

    gl_Position = vec4(positions[gl_VertexIndex], 1.0);
    outColor = colors[gl_VertexIndex];
}