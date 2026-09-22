#pragma once
namespace raster_shaders {
inline constexpr const char* vertex = R"GLSL(#version 130
in vec3 aPos, aNormal, aTangent, aBitangent;
in vec2 aUV;
uniform mat4 uModel, uView, uProj;
uniform mat3 uNormalMatrix;
out vec3 vWorld, vNormal, vTangent, vBitangent;
out vec2 vUV;
void main() {
    vec4 p = uModel * vec4(aPos, 1);
    vWorld = p.xyz;
    vNormal = uNormalMatrix * aNormal;
    vTangent = mat3(uModel) * aTangent;
    vBitangent = mat3(uModel) * aBitangent;
    vUV = aUV;
    gl_Position = uProj * uView * p;
}
)GLSL";
inline constexpr const char* material = R"GLSL(#version 130
const float PI = 3.14159265358979323846;
in vec3 vWorld, vNormal, vTangent, vBitangent;
in vec2 vUV;
uniform vec4 uBase, uParameters, uF0, uEmission, uAlpha;
uniform ivec4 uPrograms[5];
uniform sampler2D uInstructions;
uniform sampler2D uTextures[12];
uniform int uTextureChannels[12];
struct Instruction { ivec4 code, inputs, channels; vec4 value, defaults; };
vec3 safeNormal(vec3 value, vec3 fallback) {
    float squared = dot(value, value);
    return squared > 1e-20 ? value * inversesqrt(squared) : fallback;
}
const int NODE_OUTPUT = 0;
const int NODE_TEXTURE = 1;
const int NODE_SCALAR = 2;
const int NODE_VECTOR = 3;
const int NODE_TEXCOORD = 4;
const int NODE_ADD = 5;
const int NODE_SUBTRACT = 6;
const int NODE_MULTIPLY = 7;
const int NODE_DIVIDE = 8;
const int NODE_LERP = 9;
const int NODE_ONE_MINUS = 10;
const int NODE_SATURATE = 11;
const int NODE_POWER = 12;
const int NODE_REROUTE = 13;
const int MODEL_LAMBERT = 0;
const int MODEL_METAL = 1;
const int MODEL_GLASS = 2;
const int MODEL_EMISSION = 3;
const int MODEL_ISOTROPIC = 4;
const int MODEL_PBR = 5;

vec3 finiteColour(vec3 c) {
    for (int k = 0; k < 3; ++k) {
        if (isnan(c[k]) || isinf(c[k])) {
            c[k] = 0;
        }
    }
    return c;
}
vec3 srgb(vec3 c) {
    c = clamp(c, 0, 1);
    return mix(pow((c + .055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(.04045)));
}

struct Value {
    vec4 colour, raw;
};
Value scalarValue(float x) {
    return Value(vec4(vec3(x), 1), vec4(vec3(x), x));
}
Value channelValue(Value v, int c) {
    if (c == 0) {
        return v;
    }
    return scalarValue(c <= 3 ? v.raw[c - 1] : c == 4 ? v.colour.a : v.raw.a);
}

// Constant sampler indices retain compatibility with OpenGL 3.0.
vec4 sampleTexture(int id, vec2 uv) {
    if (id == 0) return texture(uTextures[0], uv);
    else if (id == 1) return texture(uTextures[1], uv);
    else if (id == 2) return texture(uTextures[2], uv);
    else if (id == 3) return texture(uTextures[3], uv);
    else if (id == 4) return texture(uTextures[4], uv);
    else if (id == 5) return texture(uTextures[5], uv);
    else if (id == 6) return texture(uTextures[6], uv);
    else if (id == 7) return texture(uTextures[7], uv);
    else if (id == 8) return texture(uTextures[8], uv);
    else if (id == 9) return texture(uTextures[9], uv);
    else if (id == 10) return texture(uTextures[10], uv);
    else if (id == 11) return texture(uTextures[11], uv);
    return vec4(1,0,1,1);
}
Value textureValue(int id, vec2 uv, int flags) {
    if (id < 0 || id >= 12) return Value(vec4(1,0,1,1), vec4(1,0,1,1));
    int info = uTextureChannels[id], channels = info & 255;
    vec4 texel = sampleTexture(id, vec2(uv.x, 1 - uv.y));
    float alpha = clamp(channels == 1 ? texel.r : (channels == 2 || channels == 4) ? texel.a : 1, 0, 1);
    float mask = channels == 3 ? dot(texel.rgb, vec3(.2126,.7152,.0722)) : alpha;
    vec3 color = ((flags & 1) != 0 && (info & 256) == 0) ? srgb(texel.rgb) : texel.rgb;
    return Value(vec4(color, alpha), vec4((flags & 2) != 0 ? color : texel.rgb, mask));
}
Value evaluate(ivec4 program, vec2 uv) {
    Value registers[32];
    for (int pc = 0; pc < program.y; ++pc) {
        int row = program.x + pc;
        Instruction op;
        op.code = ivec4(texelFetch(uInstructions, ivec2(0, row), 0));
        op.inputs = ivec4(texelFetch(uInstructions, ivec2(1, row), 0));
        op.channels = ivec4(texelFetch(uInstructions, ivec2(2, row), 0));
        op.value = texelFetch(uInstructions, ivec2(3, row), 0);
        op.defaults = texelFetch(uInstructions, ivec2(4, row), 0);
        Value a = op.inputs.x < 0 ? scalarValue(op.defaults.x)
                                  : channelValue(registers[op.inputs.x], op.channels.x);
        Value b = op.inputs.y < 0 ? scalarValue(op.defaults.y)
                                  : channelValue(registers[op.inputs.y], op.channels.y);
        Value c = op.inputs.z < 0 ? scalarValue(op.defaults.z)
                                  : channelValue(registers[op.inputs.z], op.channels.z);
        Value result = scalarValue(0);
        int kind = op.code.x;
        if (kind == NODE_TEXTURE) {
            result = textureValue(op.code.z, op.inputs.x < 0 ? uv : fract(a.colour.xy), op.code.w);
        } else {
            if (kind == NODE_SCALAR) {
                result.colour.rgb = vec3(op.value.x);
            } else if (kind == NODE_VECTOR) {
                result.colour.rgb = op.value.xyz;
            } else if (kind == NODE_TEXCOORD) {
                result.colour.rgb = vec3(uv * op.value.xy, 0);
            } else {
                vec3 av = a.colour.rgb, bv = b.colour.rgb, t = c.colour.rgb, r = av;
                if (kind == NODE_ADD) {
                    r = av + bv;
                } else if (kind == NODE_SUBTRACT) {
                    r = av - bv;
                } else if (kind == NODE_MULTIPLY) {
                    r = av * bv;
                } else if (kind == NODE_DIVIDE) {
                    for (int k = 0; k < 3; ++k) {
                        r[k] = abs(bv[k]) < 1e-8 ? 0 : av[k] / bv[k];
                    }
                } else if (kind == NODE_LERP) {
                    r = mix(av, bv, t);
                } else if (kind == NODE_ONE_MINUS) {
                    r = 1 - av;
                } else if (kind == NODE_SATURATE) {
                    r = clamp(av, 0, 1);
                } else if (kind == NODE_POWER) {
                    for (int k = 0; k < 3; ++k) {
                        r[k] = bv[k] == 0 ? 1 : pow(max(0, av[k]), bv[k]);
                    }
                }
                result.colour = vec4(finiteColour(r), a.colour.a);
                if (kind == NODE_MULTIPLY) {
                    result.colour.a *= b.colour.a;
                }
                if (kind == NODE_LERP) {
                    result.colour.a = mix(a.colour.a, b.colour.a, c.colour.r);
                }
            }
            result.raw = vec4(result.colour.rgb, result.colour.r);
        }
        registers[op.code.y] = result;
    }
    return channelValue(registers[program.z], program.w);
}


Value baseValue() {
    return uPrograms[0].y > 0 ? evaluate(uPrograms[0], vUV) : Value(vec4(uBase.rgb,1), vec4(uBase.rgb,1));
}
void maskSurface(Value base) {
    if (int(uBase.w) != MODEL_PBR) return;
    float opacity = uPrograms[4].y > 0 ? evaluate(uPrograms[4], vUV).colour.r : base.colour.a;
    // The interactive viewport uses a stable cutout for stochastic path-traced opacity.
    if (opacity < max(uAlpha.x, .5)) discard;
}
)GLSL";
inline constexpr const char* lit = R"GLSL(
uniform vec3 uEye, uSunDirection, uSunRadiance;
uniform mat4 uShadowView, uShadowProjection;
uniform sampler2DShadow uSunShadow;
uniform samplerCube uPointShadow;
uniform bool uSunShadowEnabled;
uniform int uPointShadowIndex, uPointCount, uDebugView;
uniform vec4 uPointPosition[8], uPointRadiance[8];
uniform float uShadowFar, uShadowWorldTexel;
out vec4 FragColor;

float sunVisibility(vec3 geometricNormal) {
    if (!uSunShadowEnabled) return 1;
    // Offset in world units so bias scales with the fitted light frustum.
    vec3 p = vWorld + geometricNormal * uShadowWorldTexel * .7;
    vec4 projected = uShadowProjection * uShadowView * vec4(p,1);
    vec3 coord = projected.xyz / projected.w * .5 + .5;
    if (any(lessThan(coord,vec3(0))) || any(greaterThan(coord,vec3(1)))) return 1;
    float visibility = 0;
    float bias = .00015;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        visibility += texture(uSunShadow, vec3(coord.xy + vec2(x,y) / 2048.0, coord.z - bias));
    }
    return visibility / 9;
}
vec3 cubeTexelDirection(vec3 direction) {
    vec3 magnitude = abs(direction);
    int face;
    vec2 uv;
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
        face = direction.x >= 0 ? 0 : 1;
        uv = vec2(face == 0 ? -direction.z : direction.z, -direction.y) / magnitude.x;
    } else if (magnitude.y >= magnitude.z) {
        face = direction.y >= 0 ? 2 : 3;
        uv = vec2(direction.x, face == 2 ? direction.z : -direction.z) / magnitude.y;
    } else {
        face = direction.z >= 0 ? 4 : 5;
        uv = vec2(face == 4 ? direction.x : -direction.x, -direction.y) / magnitude.z;
    }
    uv = (clamp(floor((uv*.5+.5)*512.0),vec2(0),vec2(511))+.5)/512.0*2-1;
    if (face == 0) return normalize(vec3(1,-uv.y,-uv.x));
    if (face == 1) return normalize(vec3(-1,-uv.y,uv.x));
    if (face == 2) return normalize(vec3(uv.x,1,uv.y));
    if (face == 3) return normalize(vec3(uv.x,-1,-uv.y));
    if (face == 4) return normalize(vec3(uv.x,-uv.y,1));
    return normalize(vec3(-uv.x,-uv.y,-1));
}
float pointVisibility(int index, vec3 geometricNormal) {
    if (index != uPointShadowIndex) return 1;
    vec3 delta = vWorld - uPointPosition[index].xyz;
    float distance = length(delta);
    if (distance >= uShadowFar) return 1;
    float bias = max(.001, distance * .0003);
    vec3 axis = safeNormal(delta,vec3(0,0,1));
    vec3 tangent = safeNormal(cross(axis, abs(axis.y) < .9 ? vec3(0,1,0) : vec3(1,0,0)), vec3(1,0,0));
    vec3 bitangent = cross(axis,tangent);
    float visible = 0;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        vec3 direction = cubeTexelDirection(delta + (tangent*x + bitangent*y) * distance / 512.0);
        float nearest = texture(uPointShadow, direction).r * uShadowFar;
        // Compare each tap against the receiver's tangent plane, not the center
        // depth. This avoids rings on sloped floors without detaching shadows.
        float planeCosine = dot(geometricNormal,direction);
        float receiver = abs(planeCosine) > .001 ? dot(geometricNormal,delta)/planeCosine : distance;
        visible += receiver - bias <= nearest ? 1 : 0;
    }
    return visible / 9;
}
vec3 fresnel(float cosine, vec3 f0) { return f0 + (1-f0) * pow(1-clamp(cosine,0,1),5); }
vec3 directLight(vec3 n, vec3 v, vec3 l, vec3 base, vec3 f0, float metal, float roughness, int model) {
    float nl = max(dot(n,l),0), nv = max(dot(n,v),0);
    if (nl <= 0 || nv <= 0) return vec3(0);
    if (model == MODEL_LAMBERT || model == MODEL_ISOTROPIC) return base * nl / PI;
    vec3 h = safeNormal(l+v,n);
    float nh = max(dot(n,h),0), vh = max(dot(v,h),0);
    float a = max(roughness*roughness,.0025), a2 = a*a;
    float denominator = 1 - nh*nh + a2*nh*nh;
    float distribution = a2 / (PI*denominator*denominator);
    float rootV = sqrt(a2+(1-a2)*nv*nv), rootL = sqrt(a2+(1-a2)*nl*nl);
    float geometryOverFourNvNl = 1 / max((nv+rootV)*(nl+rootL),1e-8);
    vec3 f = fresnel(vh,f0);
    vec3 diffuse = (1-f)*(1-metal)*base/PI;
    return (diffuse + distribution*geometryOverFourNvNl*f)*nl;
}
vec3 displayColor(vec3 linearColor) {
    // Linear HDR lighting, Reinhard shoulder, then the sRGB display transfer.
    vec3 c = max(finiteColour(linearColor),0);
    c = c / (1+c);
    return mix(1.055*pow(c,vec3(1.0/2.4))-.055, c*12.92, lessThanEqual(c,vec3(.0031308)));
}
void main() {
    Value surface = baseValue();
    maskSurface(surface);
    int model = int(uBase.w);
    vec3 base = clamp(surface.colour.rgb,0,1);
    vec3 smoothNormal = safeNormal(vNormal,vec3(0,1,0));
    vec3 v = safeNormal(uEye-vWorld, smoothNormal);
    if (dot(smoothNormal,v) < 0) smoothNormal = -smoothNormal;
    vec3 geometricNormal = safeNormal(cross(dFdx(vWorld),dFdy(vWorld)),smoothNormal);
    if (dot(geometricNormal,v) < 0) geometricNormal = -geometricNormal;
    vec3 n = smoothNormal;
    if (uPrograms[3].y > 0) {
        vec3 t = safeNormal(vTangent - n*dot(n,vTangent),safeNormal(cross(n,vec3(.123,.456,.789)),vec3(1,0,0)));
        vec3 b = cross(n,t) * (dot(cross(n,t),vBitangent) < 0 ? -1 : 1);
        vec3 local = evaluate(uPrograms[3],vUV).colour.rgb * 2 - 1;
        local.xy *= uParameters.z;
        n = safeNormal(t*local.x + b*local.y + n*local.z,n);
        if (dot(n,geometricNormal) <= 0) n = geometricNormal;
    }
    if (uDebugView == 1) { FragColor = vec4(base,1); return; }
    if (uDebugView == 2) { FragColor = vec4(n*.5+.5,1); return; }
    if (model == MODEL_EMISSION) {
        FragColor = vec4(displayColor((uPrograms[0].y > 0 ? surface.colour.rgb : uEmission.rgb)*uEmission.w),1);
        return;
    }
    float metal = clamp(uPrograms[1].y > 0 ? evaluate(uPrograms[1],vUV).colour.r : uParameters.x,0,1);
    float roughness = clamp(uPrograms[2].y > 0 ? evaluate(uPrograms[2],vUV).colour.r : uParameters.y,0,1);
    if (model == MODEL_METAL) { metal = 1; roughness = sqrt(clamp(uF0.w,0,1)); }
    vec3 f0 = mix(clamp(uF0.rgb,0,1),base,metal);
    if (model == MODEL_GLASS) {
        float f = (uParameters.w-1)/(uParameters.w+1);
        f0 = vec3(f*f); metal = 0; roughness = .05;
    }
    // Neutral studio fill keeps materials legible without pretending to solve indirect transport.
    float sky = .5+.5*n.y;
    vec3 reflected = reflect(-v,n);
    vec3 ambientSpecular = (model == MODEL_LAMBERT || model == MODEL_ISOTROPIC) ? vec3(0) :
        fresnel(max(dot(n,v),0),f0)*(.12+.22*(.5+.5*reflected.y));
    vec3 color = base*(1-metal)*(.07+.08*sky) + ambientSpecular;
    color += directLight(n,v,uSunDirection,base,f0,metal,roughness,model)*uSunRadiance*sunVisibility(geometricNormal);
    for (int i = 0; i < uPointCount; ++i) {
        vec3 delta = uPointPosition[i].xyz-vWorld;
        float distance = length(delta), range = uPointPosition[i].w;
        if (range > 0 && distance > range) continue;
        color += directLight(n,v,safeNormal(delta,n),base,f0,metal,roughness,model) *
                 uPointRadiance[i].rgb / max(distance*distance,.01) * pointVisibility(i,geometricNormal);
    }
    FragColor = vec4(displayColor(color),1);
}
)GLSL";
inline constexpr const char* shadow = R"GLSL(
uniform bool uPointShadowPass;
uniform vec3 uShadowPosition;
uniform float uShadowFar;
void main() {
    maskSurface(baseValue());
    gl_FragDepth = uPointShadowPass ? length(vWorld - uShadowPosition) / uShadowFar : gl_FragCoord.z;
}
)GLSL";
} // namespace raster_shaders
