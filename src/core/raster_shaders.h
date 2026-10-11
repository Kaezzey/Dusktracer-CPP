#pragma once
namespace raster_shaders {
// Diffusion taps per light; must match DIFFUSION_TAPS in the lit shader.
inline constexpr int diffusion_taps = 36;
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
inline constexpr const char* version = "#version 130\n";
inline constexpr const char* material = R"GLSL(
const float PI = 3.14159265358979323846;
#ifdef MATERIAL_SPHERE_PREVIEW
vec3 vWorld, vNormal, vTangent, vBitangent;
vec2 vUV;
#else
in vec3 vWorld, vNormal, vTangent, vBitangent;
in vec2 vUV;
#endif
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
const int NODE_CONSTANT = -1;
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
        if (kind == NODE_CONSTANT) {
            result = Value(op.value, op.defaults);
        } else if (kind == NODE_TEXTURE) {
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
// Subsurface lanes of the shared material record (see gpu_scene.h).
uniform vec4 uSubsurface, uScatterTint, uSubsurfaceDistances, uSubsurfaceColour, uSpecularLobes, uProfileTint;
// Light-space surface maps written by the shadow pass: x = depth toward the
// sun or distance from the point light, y = object id, z = cosine to the light.
// Packed as RG32F: x = depth or distance, y = object id + 0.99 * cosine.
uniform sampler2D uSunSurface;
uniform samplerCube uPointSurface;
uniform bool uSurfaceMaps;
uniform float uObjectId;
// Uniform environment radiance: the renderer's Sky / Background colour.
uniform vec3 uEnvironment;
out vec4 FragColor;

float sunVisibility(vec3 geometricNormal) {
    if (!uSunShadowEnabled) return 1;
    // Offset in world units so bias scales with the fitted light frustum. At
    // grazing angles one texel spans a long strip of surface, so the offset
    // grows until the lookup clears the object's own light-space silhouette.
    float lit = clamp(dot(geometricNormal, uSunDirection), 0, 1);
    vec3 p = vWorld + geometricNormal * uShadowWorldTexel * (.7 + 2 * sqrt(1 - lit * lit));
    // Each tap compares against the receiver's tangent plane, not the center
    // depth, so sloped lit surfaces do not shadow themselves.
    vec3 u = normalize(cross(uSunDirection, abs(uSunDirection.y) < .9 ? vec3(0,1,0) : vec3(1,0,0)));
    vec3 v = cross(uSunDirection, u);
    // The plane must keep its true slope near grazing: a shallower plane puts
    // taps on the rising side inside the object, which then shadows itself.
    float facing = max(dot(geometricNormal, uSunDirection), .05);
    float visibility = 0;
    float bias = .00015;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        vec3 offset = (u*float(x) + v*float(y)) * uShadowWorldTexel;
        float rise = clamp(dot(geometricNormal, offset) / facing, -12 * uShadowWorldTexel, 12 * uShadowWorldTexel);
        vec3 q = p + offset - uSunDirection * rise;
        vec4 projected = uShadowProjection * uShadowView * vec4(q,1);
        vec3 coord = projected.xyz / projected.w * .5 + .5;
        if (any(lessThan(coord,vec3(0))) || any(greaterThan(coord,vec3(1)))) visibility += 1;
        else visibility += texture(uSunShadow, vec3(coord.xy, coord.z - bias));
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
)GLSL" // MSVC limits each literal piece to 16 KiB; adjacent pieces concatenate.
R"GLSL(
// ---- Subsurface: the same functions and integrals as the path tracers ----
float entryTransmission(float cosine, float eta) {
    cosine = clamp(cosine,0,1);
    if (eta == 1) return 1;
    float transmitted = sqrt(max(0,1-(1-cosine*cosine)/(eta*eta)));
    float rp = (eta*cosine-transmitted)/(eta*cosine+transmitted);
    float rs = (cosine-eta*transmitted)/(cosine+eta*transmitted);
    return 1-.5*(rp*rp+rs*rs);
}
// Entry lighting per unit radiance, cosine included (sss::entry_directional).
float entryLighting(float cosine) {
    return cosine > 0 ? cosine*entryTransmission(cosine,uSubsurface.z)*uSubsurface.w : 0;
}
float diffusionProfile(float radius, float distance) {
    radius = max(radius,1e-12);
    return (exp(-radius/distance)+exp(-radius/(3*distance)))/(8*PI*distance*radius);
}
vec3 diffusionProfiles(float radius) {
    return vec3(diffusionProfile(radius,uSubsurfaceDistances.x),diffusionProfile(radius,uSubsurfaceDistances.y),
                diffusionProfile(radius,uSubsurfaceDistances.z));
}
float unrealTransmission(float nh, float back, float opacity) {
    float inScatter = pow(clamp(back,0,1),12)*(3+(.1-3)*opacity);
    float backScatter = clamp(nh*opacity+1-opacity,0,1)/(2*PI);
    return backScatter+(1-backScatter)*inScatter;
}
float sameObject(float id) { return abs(id-uObjectId) < .5 ? 1 : 0; }
vec3 surfaceRecord(vec2 texel) {
    float id = floor(texel.y);
    return vec3(texel.x,id,(texel.y-id)/.99);
}
vec3 perpendicular(vec3 axis) {
    return safeNormal(cross(axis,abs(axis.y) < .9 ? vec3(0,1,0) : vec3(1,0,0)),vec3(1,0,0));
}
// The material preview sphere is the unit sphere at the origin, so its
// light-space surfaces are analytic and need no maps.
bool sunSurface(vec3 q, out vec3 record) {
#ifdef MATERIAL_SPHERE_PREVIEW
    vec3 lateral = q-dot(q,uSunDirection)*uSunDirection;
    float squared = dot(lateral,lateral);
    if (squared >= 1) return false;
    float height = sqrt(1-squared);
    record = vec3(height,uObjectId,height);
    return true;
#else
    if (!uSurfaceMaps) return false;
    vec4 projected = uShadowProjection*uShadowView*vec4(q,1);
    vec2 uv = projected.xy/projected.w*.5+.5;
    if (any(lessThan(uv,vec2(0))) || any(greaterThanEqual(uv,vec2(1)))) return false;
    record = surfaceRecord(texelFetch(uSunSurface,ivec2(uv*vec2(textureSize(uSunSurface,0))),0).xy);
    return record.y > 0;
#endif
}
bool pointMapped(int index) {
#ifdef MATERIAL_SPHERE_PREVIEW
    return true;
#else
    return uSurfaceMaps && index == uPointShadowIndex;
#endif
}
bool pointSurface(vec3 origin, vec3 direction, out vec3 record) {
#ifdef MATERIAL_SPHERE_PREVIEW
    vec3 d = normalize(direction);
    float b = dot(origin,d), discriminant = b*b-dot(origin,origin)+1;
    if (discriminant < 0) return false;
    float t = -b-sqrt(discriminant);
    if (t <= 0) return false;
    record = vec3(t,uObjectId,max(0,-dot(origin+t*d,d)));
    return true;
#else
    record = surfaceRecord(texture(uPointSurface,direction).xy);
    return record.y > 0;
#endif
}
// Light reaching a back face through this object's own body is attenuated
// by exp(-thickness/distance), thickness measured to its surface nearest the
// light, exactly as bodyVisibility/body_transmittance in the path tracers.
float bodySun(vec3 x) {
#ifdef MATERIAL_SPHERE_PREVIEW
    const int extent = 0;
#else
    const int extent = 1;
    if (!uSurfaceMaps) return 1;
#endif
    vec3 u = perpendicular(uSunDirection), v = cross(uSunDirection,u);
    float depth = dot(x,uSunDirection), visible = 0;
    for (int j = -extent; j <= extent; ++j) for (int i = -extent; i <= extent; ++i) {
        vec3 record;
        // A surface at or behind x, such as the floor beyond the silhouette, blocks nothing.
        if (!sunSurface(x+(u*float(i)+v*float(j))*uShadowWorldTexel,record) || record.x <= depth) visible += 1;
        else visible += sameObject(record.y)*exp(-(record.x-depth)/uSpecularLobes.w);
    }
    return visible/float((2*extent+1)*(2*extent+1));
}
float bodyPoint(int index, vec3 x) {
    if (!pointMapped(index)) return 1;
    vec3 origin = uPointPosition[index].xyz, axis = x-origin;
    float distance = length(axis);
    if (distance <= 0) return 1;
    vec3 record;
    if (!pointSurface(origin,axis,record) || record.x >= distance) return 1;
    return sameObject(record.y)*exp(-(distance-record.x)/uSpecularLobes.w);
}
// Per-material taps computed once on the CPU (raster_preview::upload_diffusion_taps):
// xy = radius times the tap's golden-angle direction, z = radius,
// w = 1/(channel-mixture density * tap count). Radii are stratified quantiles
// of each channel's Burley distribution, as the tracers sample them.
const int DIFFUSION_TAPS = 36; // raster_shaders::diffusion_taps
uniform vec4 uDiffusionTaps[DIFFUSION_TAPS];
// Integral of the Burley profile times entry lighting over the lit surface
// of this object and material, per unit sun radiance. Taps lie in the
// light's plane; the projection Jacobian cancels the entry cosine. Taps
// closer than the shadow map can resolve use the receiver's tangent plane,
// lit with its filtered visibility, so the map's texel staircase along the
// terminator never shows; farther taps read the map, whose depth snaps to
// that plane within shadow-map precision.
vec3 diffusionSun(vec3 x, vec3 g, mat2 rotation, float localVisible) {
    vec3 u = perpendicular(uSunDirection), v = cross(uSunDirection,u);
    float depth = dot(x,uSunDirection), facing = dot(g,uSunDirection), gu = dot(g,u), gv = dot(g,v);
    float localWeight = facing > 0 ? localVisible : 0;
#ifdef MATERIAL_SPHERE_PREVIEW
    float tolerance = 0, nearField = 0;
#else
    float tolerance = uShadowWorldTexel*(1+1.5*sqrt(max(0,1-facing*facing))/max(facing,1e-3));
    float nearField = 1.5*uShadowWorldTexel;
    // The sun projection is orthographic, so tap coordinates are linear in the offset.
    mat4 shadow = uShadowProjection*uShadowView;
    vec2 base = (shadow*vec4(x,1)).xy, du = (shadow*vec4(u,0)).xy, dv = (shadow*vec4(v,0)).xy;
    vec2 size = vec2(textureSize(uSunSurface,0));
#endif
    vec3 sum = vec3(0);
    // Branch-free taps let the GPU keep many surface fetches in flight.
    for (int k = 0; k < DIFFUSION_TAPS; ++k) {
        vec4 tap = uDiffusionTaps[k];
        vec2 o = rotation*tap.xy;
        vec3 record;
#ifdef MATERIAL_SPHERE_PREVIEW
        float weight = sunSurface(x+o.x*u+o.y*v,record) ? sameObject(record.y) : 0;
#else
        vec2 uv = (base+o.x*du+o.y*dv)*.5+.5;
        float inside = all(greaterThanEqual(uv,vec2(0))) && all(lessThan(uv,vec2(1))) ? 1 : 0;
        record = surfaceRecord(texelFetch(uSunSurface,ivec2(clamp(uv,0,1)*(size-1)),0).xy);
        float weight = inside*sameObject(record.y);
#endif
        float rise = record.x-depth, plane = -(o.x*gu+o.y*gv)/max(facing,.05);
        rise = facing > .05 && abs(rise-plane) < tolerance ? plane : rise;
        float cosine = record.z;
        if (tap.z < nearField) {
            rise = plane;
            cosine = max(facing,0);
            weight = localWeight;
        }
        sum += diffusionProfiles(sqrt(tap.z*tap.z+rise*rise))*(weight*entryTransmission(cosine,uSubsurface.z)*tap.w);
    }
    return sum*uSubsurface.w;
}
// The same integral for a point light, per unit intensity. Taps lie in the
// plane through x perpendicular to the light ray; D/(D^2+r^2)^1.5 converts
// that plane's area to solid angle at the light.
vec3 diffusionPoint(int index, vec3 x, vec3 g, mat2 rotation, float localVisible) {
    vec3 origin = uPointPosition[index].xyz, axis = x-origin;
    float distance = length(axis);
    if (distance <= 0) return vec3(0);
    vec3 a = axis/distance, u = perpendicular(a), v = cross(a,u);
    float facing = -dot(g,a), localWeight = facing > 0 ? localVisible : 0;
#ifdef MATERIAL_SPHERE_PREVIEW
    float tolerance = 0, nearField = 0;
#else
    float texel = distance*(2.0/512.0);
    float tolerance = texel*(1+1.5*sqrt(max(0,1-facing*facing))/max(facing,1e-3)), nearField = 1.5*texel;
#endif
    vec3 sum = vec3(0);
    for (int k = 0; k < DIFFUSION_TAPS; ++k) {
        vec4 tap = uDiffusionTaps[k];
        vec2 o = rotation*tap.xy;
        vec3 direction = axis+o.x*u+o.y*v, record;
        float squared = distance*distance+tap.z*tap.z;
        vec3 d = direction*inversesqrt(squared);
        float denominator = dot(g,d), plane = dot(g,x-origin)/min(denominator,-.05);
        float weight = pointSurface(origin,direction,record) ? sameObject(record.y) : 0;
        float t = record.x, cosine = record.z;
        if (facing > .05 && denominator < -.05 && abs(t-plane) < tolerance) t = plane;
        if (tap.z < nearField) {
            t = plane;
            cosine = max(facing,0);
            weight = localWeight;
        }
        sum += diffusionProfiles(length(origin+t*d-x))*
               (weight*entryTransmission(cosine,uSubsurface.z)*distance*inversesqrt(squared)/squared*tap.w);
    }
    return sum*uSubsurface.w;
}
vec3 fresnel(float cosine, vec3 f0) { return f0 + (1-f0) * pow(1-clamp(cosine,0,1),5); }
float smithRoot(float cosine, float alpha) { return sqrt(alpha*alpha+(1-alpha*alpha)*cosine*cosine); }
float ggx(float nh, float alpha) {
    float a2 = alpha*alpha, d = 1-nh*nh+a2*nh*nh;
    return nh > 0 ? a2/(PI*d*d) : 0;
}
// Path-traced PBR surface lobes times cosine. Delta-smooth lobes keep a
// narrow viewport highlight where the tracer reflects the sun disc instead.
vec3 pbrLight(vec3 n, vec3 g, vec3 v, vec3 l, vec3 base, vec3 f0, float metal, float fraction, vec4 alphas) {
    float nl = dot(n,l), nv = dot(n,v);
    if (dot(g,l) <= 0 || dot(g,v) <= 0 || nl <= 0 || nv <= 0) return vec3(0);
    nl = min(nl,1); nv = min(nv,1);
    vec3 f = base*((1-metal)*(1-fraction)/PI)*(1-fresnel(nv,f0))*(1-fresnel(nl,f0));
    vec3 a = max(alphas.xyz,vec3(alphas.x < 1e-4 ? .0025 : 0));
    vec3 h = safeNormal(v+l,n);
    float nh = clamp(dot(n,h),0,1), d = ggx(nh,a.x);
    if (alphas.w > 0) d += (ggx(nh,a.y)-d)*alphas.w;
    f += fresnel(dot(v,h),f0)*(d*.5/(nl*smithRoot(nv,a.z)+nv*smithRoot(nl,a.z)));
    return f*nl;
}
vec3 transmissionLight(vec3 n, vec3 g, vec3 v, vec3 l, vec3 colour) {
    if (uSpecularLobes.w <= 0 || dot(g,v) <= 0 || dot(n,v) <= 0) return vec3(0);
    return colour*unrealTransmission(dot(n,safeNormal(v+l,n)),-dot(l,v),clamp(uSubsurfaceColour.w,0,1));
}
float interleavedNoise(vec2 p) { return fract(52.9829189*fract(dot(p,vec2(.06711056,.00583715)))); }
)GLSL"
R"GLSL(
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
void shadeSurface() {
    Value surface = baseValue();
    maskSurface(surface);
    int model = int(uBase.w);
    vec3 base = clamp(surface.colour.rgb,0,1);
    vec3 smoothNormal = safeNormal(vNormal,vec3(0,1,0));
    vec3 v = safeNormal(uEye-vWorld, smoothNormal);
    if (dot(smoothNormal,v) < 0) smoothNormal = -smoothNormal;
#ifdef MATERIAL_SPHERE_PREVIEW
    vec3 geometricNormal = smoothNormal;
#else
    vec3 geometricNormal = safeNormal(cross(dFdx(vWorld),dFdy(vWorld)),smoothNormal);
    if (dot(geometricNormal,v) < 0) geometricNormal = -geometricNormal;
#endif
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
    if (uDebugView == 4) { FragColor = vec4(vec3(roughness),1); return; }
    vec3 f0 = mix(clamp(uF0.rgb,0,1),base,metal);
    if (model == MODEL_GLASS) {
        float f = (uParameters.w-1)/(uParameters.w+1);
        f0 = vec3(f*f); metal = 0; roughness = .05;
    }
    bool pbr = model == MODEL_PBR;
    // The tracers' geometric normal is smooth on analytic spheres; the raster
    // sphere's facet normal only locates shadow-map lookups.
    vec3 surfaceNormal = smoothNormal;
    // Dual specular lobes (Unreal Subsurface Profile) share the tracers' roughness mapping.
    vec4 alphas = vec4(vec3(roughness*roughness),0);
    if (pbr && uSpecularLobes.z > 0) {
        float r0 = clamp(roughness*uSpecularLobes.x,.02,1), r1 = clamp(roughness*uSpecularLobes.y,.02,1);
        float mean = r0+(r1-r0)*clamp(uSpecularLobes.z,0,1);
        alphas = vec4(r0*r0,r1*r1,mean*mean,clamp(uSpecularLobes.z,0,1));
    }
    float fraction = pbr ? clamp(uSubsurface.x,0,1) : 0;
    bool diffusion = fraction > 0, body = pbr && uSpecularLobes.w > 0;
    // Path-traced diffusion leaves through (1-F) with the tint applied once.
    vec3 tint = uScatterTint.w != 0 ? clamp(uScatterTint.rgb,0,1) : base*clamp(uProfileTint.rgb,0,1);
    vec3 exitWeight = (1-fresnel(dot(n,v),f0))*((1-metal)*fraction)*tint;
    float angle = 2*PI*interleavedNoise(gl_FragCoord.xy);
    mat2 rotation = mat2(cos(angle),sin(angle),-sin(angle),cos(angle));
    // Ambient light: the tracers' uniform environment (diffuse substrate behind
    // (1-F) and a Fresnel reflection of the sky colour), plus a small diffuse
    // fill standing in for bounce light, which the viewport does not trace.
    float sky = .5+.5*n.y;
    vec3 fill = vec3(.07+.08*sky)+uEnvironment;
    vec3 surfaceFresnel = fresnel(max(dot(n,v),0),f0);
    bool specular = model != MODEL_LAMBERT && model != MODEL_ISOTROPIC;
    vec3 color = base*(1-metal)*(pbr ? 1-surfaceFresnel : vec3(1))*fill +
                 (specular ? surfaceFresnel*uEnvironment : vec3(0));
    // Like Unreal, ambient light treats the subsurface colour as extra diffuse.
    vec3 subsurfaceColour = clamp(uSubsurfaceColour.rgb,0,1)*(uProfileTint.w > 0 ? base : vec3(1));
    // Kept within unit albedo, as the tracers' ambient lobe is.
    if (body) color += clamp(min(subsurfaceColour,1-(1-metal)*base),0,1)*fill;
    if (uDebugView == 3) color = vec3(0); // direct lighting only, for path-tracer comparisons
    vec3 l = uSunDirection;
    bool behind = dot(surfaceNormal,l) < 0;
    float sunLocal = sunVisibility(geometricNormal);
    float visible = body && behind ? bodySun(vWorld) : sunLocal;
    vec3 surfaceLight = pbr ? pbrLight(n,surfaceNormal,v,l,base,f0,metal,fraction,alphas)+transmissionLight(n,surfaceNormal,v,l,subsurfaceColour)
                            : directLight(n,v,l,base,f0,metal,roughness,model);
    color += surfaceLight*uSunRadiance*visible;
    // A kernel inside one shadow texel (97% of Burley energy lies within 10d)
    // integrates to local entry lighting; the taps could not resolve it.
#ifdef MATERIAL_SPHERE_PREVIEW
    bool sunMapped = true;
#else
    bool sunMapped = uSurfaceMaps && 10*uSubsurface.y > uShadowWorldTexel;
#endif
    if (diffusion) {
        color += exitWeight*uSunRadiance*(sunMapped ? diffusionSun(vWorld,surfaceNormal,rotation,sunLocal)
            : vec3(entryLighting(dot(surfaceNormal,l))*sunLocal));
    }
    for (int i = 0; i < uPointCount; ++i) {
        vec3 delta = uPointPosition[i].xyz-vWorld;
        float distance = length(delta), range = uPointPosition[i].w;
        if (range > 0 && distance > range) continue;
        l = safeNormal(delta,n);
        behind = dot(surfaceNormal,l) < 0;
        float pointLocal = pointVisibility(i,geometricNormal);
        visible = body && behind ? bodyPoint(i,vWorld) : pointLocal;
        vec3 irradiance = uPointRadiance[i].rgb / max(distance*distance,.01);
        surfaceLight = pbr ? pbrLight(n,surfaceNormal,v,l,base,f0,metal,fraction,alphas)+transmissionLight(n,surfaceNormal,v,l,subsurfaceColour)
                           : directLight(n,v,l,base,f0,metal,roughness,model);
        color += surfaceLight*irradiance*visible;
        if (diffusion) {
#ifdef MATERIAL_SPHERE_PREVIEW
            bool resolved = true;
#else
            bool resolved = 10*uSubsurface.y > distance*(2.0/512.0);
#endif
            color += exitWeight*(pointMapped(i) && resolved ? uPointRadiance[i].rgb*diffusionPoint(i,vWorld,surfaceNormal,rotation,pointLocal)
                : irradiance*(entryLighting(dot(surfaceNormal,l))*pointLocal));
        }
    }
    FragColor = vec4(uDebugView == 3 ? max(finiteColour(color),0) : displayColor(color),1);
}
#ifndef MATERIAL_SPHERE_PREVIEW
void main() { shadeSurface(); }
#endif
)GLSL";
inline constexpr const char* shadow = R"GLSL(
uniform bool uPointShadowPass;
uniform vec3 uShadowPosition, uLightAxis;
uniform float uShadowFar, uObjectId;
// Light-space surface record used by subsurface shading; see uSunSurface.
out vec4 SurfaceData;
void main() {
    maskSurface(baseValue());
    vec3 toLight = uPointShadowPass ? safeNormal(uShadowPosition - vWorld, uLightAxis) : uLightAxis;
    vec3 geometric = safeNormal(cross(dFdx(vWorld), dFdy(vWorld)), toLight);
    float distance = length(vWorld - uShadowPosition);
    // Entry cosines use the interpolated normal, as the tracers do on analytic spheres.
    vec3 smoothNormal = safeNormal(vNormal, geometric);
    SurfaceData = vec4(uPointShadowPass ? distance : dot(vWorld, uLightAxis),
                       uObjectId + .99 * abs(dot(smoothNormal, toLight)), 0, 0);
    gl_FragDepth = uPointShadowPass ? distance / uShadowFar : gl_FragCoord.z;
}
)GLSL";
} // namespace raster_shaders
