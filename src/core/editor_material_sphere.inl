#include "material_sphere_shaders.h"

namespace material_sphere {
static raster_preview::program shader;
static GLuint framebuffer = 0, vertex_array = 0;
static bool attempted = false;
static uint64_t draws = 0;

// Preview rendering runs while building the UI. Restore the caller's GL state,
// including texture bindings, so neither the viewport nor ImGui inherits it.
struct saved_gl_state {
    GLint draw_fbo, read_fbo, viewport[4], program, vao, active_texture, unpack_alignment;
    GLint textures[13], polygon_mode[2];
    GLfloat clear_color[4];
    GLboolean color_mask[4];
    const GLenum capabilities[6] = {GL_BLEND,        GL_DEPTH_TEST,       GL_CULL_FACE,
                                    GL_SCISSOR_TEST, GL_FRAMEBUFFER_SRGB, GL_RASTERIZER_DISCARD};
    GLboolean enabled[6];

    saved_gl_state() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
        glGetIntegerv(GL_POLYGON_MODE, polygon_mode);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        for (int i = 0; i < 13; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
        }
        for (int i = 0; i < 6; ++i) {
            enabled[i] = glIsEnabled(capabilities[i]);
            glDisable(capabilities[i]);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    ~saved_gl_state() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glUseProgram(program);
        glBindVertexArray(vao);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
        if (polygon_mode[0] == polygon_mode[1]) {
            glPolygonMode(GL_FRONT_AND_BACK, polygon_mode[0]);
        } else {
            // Separate face modes occur only in compatibility contexts.
            glPolygonMode(GL_FRONT, polygon_mode[0]);
            glPolygonMode(GL_BACK, polygon_mode[1]);
        }
        glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
        glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        for (int i = 0; i < 6; ++i) {
            if (enabled[i]) {
                glEnable(capabilities[i]);
            }
        }
        for (int i = 0; i < 13; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, textures[i]);
        }
        glActiveTexture(active_texture);
    }
};

static bool initialize() {
    if (attempted) {
        return shader.id != 0;
    }
    attempted = true;
    BuildRasterShader();
    const std::string source = std::string(raster_shaders::version) + "#define MATERIAL_SPHERE_PREVIEW\n" +
                               raster_shaders::material + raster_shaders::lit +
                               material_sphere_shaders::fragment;
    shader.id = CompileShader(material_sphere_shaders::vertex, source.c_str());
    GLint linked = GL_FALSE;
    glGetProgramiv(shader.id, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(shader.id);
        shader.id = 0;
        return false;
    }
    glUseProgram(shader.id);
    const int units[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    glUniform1iv(shader.location("uTextures[0]"), raster_preview::texture_slots, units);
    glUniform1i(shader.location("uInstructions"), 12);
    glUniform1i(shader.location("uSunShadow"), 13);
    glUniform1i(shader.location("uPointShadow"), 14);
    glUniform1i(shader.location("uSunSurface"), 15);
    glUniform1i(shader.location("uPointSurface"), 16);
    glUniform1f(shader.location("uObjectId"), 1); // analytic preview surfaces use this id
    glUniform3f(shader.location("uEnvironment"), .2f, .2f, .2f); // neutral studio sky
    glUniform1i(shader.location("uSunShadowEnabled"), GL_FALSE);
    glUniform1i(shader.location("uPointShadowIndex"), -1);
    glUniform1i(shader.location("uDebugView"), 0);
    const vec3 key = unit_vector(vec3(-.5, .7, 1.2));
    glUniform3f(shader.location("uSunDirection"), float(key.x()), float(key.y()), float(key.z()));
    glUniform3f(shader.location("uSunRadiance"), 3.8f, 3.6f, 3.4f);
    glUniform1i(shader.location("uPointCount"), 1);
    const vec3 fill = 20 * unit_vector(vec3(.8, .1, .4));
    glUniform4f(shader.location("uPointPosition[0]"), float(fill.x()), float(fill.y()), float(fill.z()), 0);
    glUniform4f(shader.location("uPointRadiance[0]"), 320, 400, 520, 0);
    glGenFramebuffers(1, &framebuffer);
    glGenVertexArrays(1, &vertex_array);
    return true;
}
} // namespace material_sphere

static bool RenderMaterialSpherePreview(int index) {
    using namespace material_sphere;
    auto& preview = g_sphere_preview_states[index];
    preview.gpu = false;
    saved_gl_state restore;
    if (!initialize()) {
        return false;
    }
    raster_preview::pump_textures();
    auto& material = raster_preview::get_material(index);
    // Keep the exact CPU evaluator as a fallback for graphs beyond GPU limits.
    if (!material.error.empty()) {
        return false;
    }
    for (const auto& path : material.paths) {
        raster_preview::get_texture(path);
    }
    preview.gpu = true;
    if (!preview.image.dirty && preview.texture && preview.texture_revision == raster_preview::revision &&
        preview.image.requested == preview.image.displayed) {
        return true;
    }

    glActiveTexture(GL_TEXTURE0);
    if (!preview.texture) {
        glGenTextures(1, &preview.texture);
    }
    glBindTexture(GL_TEXTURE_2D, preview.texture);
    if (preview.image.size != MATERIAL_SPHERE_SIZE) {
        raster_preview::configure_texture(preview.texture, false);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MATERIAL_SPHERE_SIZE, MATERIAL_SPHERE_SIZE, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, preview.texture, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        preview.gpu = false;
        return false;
    }
    glViewport(0, 0, MATERIAL_SPHERE_SIZE, MATERIAL_SPHERE_SIZE);
    const colour background(.028, .039, .052);
    glClearColor(float(linear_to_gamma(background.x() / (1 + background.x()))),
                 float(linear_to_gamma(background.y() / (1 + background.y()))),
                 float(linear_to_gamma(background.z() / (1 + background.z()))), 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(shader.id);
    raster_preview::bind_material(shader, index);
    const material_preview_camera camera(preview.yaw, preview.pitch, preview.zoom);
    auto vector = [&](const char* name, const vec3& value) {
        glUniform3f(shader.location(name), float(value.x()), float(value.y()), float(value.z()));
    };
    vector("uEye", camera.eye);
    vector("uPreviewOutward", camera.outward);
    vector("uPreviewRight", camera.right);
    vector("uPreviewUp", camera.up);
    glUniform1f(shader.location("uPreviewHalfHeight"), float(camera.half_height));
    glUniform1f(shader.location("uPreviewSize"), float(MATERIAL_SPHERE_SIZE));
    glBindVertexArray(vertex_array);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ++draws;
    preview.image.size = preview.image.requested_size = MATERIAL_SPHERE_SIZE;
    preview.image.requested = preview.image.displayed = ++g_preview_revision;
    preview.image.dirty = false;
    preview.texture_revision = raster_preview::revision;
    return true;
}

static void StopMaterialSpherePreview() {
    using namespace material_sphere;
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteVertexArrays(1, &vertex_array);
    glDeleteProgram(shader.id);
    framebuffer = vertex_array = 0;
    shader = {};
    attempted = false;
}
