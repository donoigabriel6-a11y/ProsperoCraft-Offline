#version 150

in vec3 Position;
in vec4 Color;
in vec2 UV0;
in uint UV1_raw_;
#define UV1 ivec2((int(UV1_raw_) << 16) >> 16, int(UV1_raw_) >> 16)

in uint UV2_raw_;
#define UV2 ivec2((int(UV2_raw_) << 16) >> 16, int(UV2_raw_) >> 16)

in vec3 Normal;

uniform sampler2D Sampler1;
uniform sampler2D Sampler2;
uniform mat4 ModelViewMat;
uniform mat4 ProjMat;
uniform vec3 Light0_Direction;
uniform vec3 Light1_Direction;

out vec4 vertexColor;
out vec4 lightMapColor;
out vec4 overlayColor;
out vec2 texCoord0;

void main() {
    gl_Position = ProjMat * ModelViewMat * vec4(Position, 1.0);
    vertexColor = Color * vec4(Light0_Direction + Light1_Direction + Normal, 1.0);
    lightMapColor = texelFetch(Sampler2, UV2 / 16, 0);
    overlayColor = texelFetch(Sampler1, UV1, 0);
    texCoord0 = UV0;
}
