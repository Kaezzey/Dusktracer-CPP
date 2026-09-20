#include "core/preview_worker.h"

static constexpr int MATERIAL_THUMB_SIZE = 128;
static std::unordered_map<int,GLuint> g_material_thumb_cache;
static preview_worker g_editor_previews;
struct preview_state {
    uint64_t requested = 0, displayed = 0;
    bool dirty = true;
    int size = 0, requested_size = 0;
    std::chrono::steady_clock::time_point submitted{};
};
static std::unordered_map<int,preview_state> g_material_preview_states;
static std::unordered_map<std::string,uint64_t> g_texture_preview_requests;
static uint64_t g_preview_revision = 0;

static preview_image RenderMaterialPreview(const scene& assets, const scene_material& description, int size) {
    // Keep only the last rendered material alive: reusable decoded images while
    // editing, without retaining every material's full-resolution maps forever.
    thread_local std::shared_ptr<material> previous;
    auto runtime = build_rt_material(assets,description);
    previous = runtime;
    hittable_list empty_world;
    preview_image image; image.width = image.height = size; image.pixels.resize(size*size*4);
    const vec3 view(0,0,1), key = unit_vector(vec3(-.5,.7,1.2)), fill = unit_vector(vec3(.8,.1,.4));
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        double sx = (2.0*(x+.5)/size-1)/.88, sy = (1-2.0*(y+.5)/size)/.88;
        colour background(.028,.039,.052), c = background;
        double r2 = sx*sx+sy*sy;
        if (r2 < 1) {
            hit_record rec{}; rec.p = rec.normal = vec3(sx,sy,std::sqrt(1-r2)); rec.front_face = true;
            rec.u = (std::atan2(-rec.p.z(),rec.p.x())+pi)/(2*pi); rec.v = std::acos(-rec.p.y())/pi;
            rec.tangent = unit_vector(vec3(rec.p.z(),0,-rec.p.x())); rec.bitangent = unit_vector(cross(rec.normal,rec.tangent));
            c = runtime->albedo(rec)*.12 + runtime->emitted(rec.u,rec.v,rec.p)
                + runtime->shade_direct(rec,view,key,colour(3.8,3.6,3.4),empty_world)
                + runtime->shade_direct(rec,view,fill,colour(.8,1,1.3),empty_world);
            double alpha = runtime->opacity_at(rec); c = alpha*c + (1-alpha)*background;
        }
        int at = (y*size+x)*4;
        for (int channel = 0; channel < 3; ++channel) {
            double v = std::max(0.0,c[channel]); v /= 1+v;
            image.pixels[at+channel] = (unsigned char)(255*linear_to_gamma(std::clamp(v,0.0,1.0)));
        }
        image.pixels[at+3] = 255;
    }
    return image;
}
static preview_image DecodeTextureThumbnail(const std::string& path) {
    int width, height, channels;
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(stbi_load(path.c_str(),&width,&height,&channels,4),stbi_image_free);
    preview_image image;
    if (!pixels || width <= 0 || height <= 0) return image;
    float scale = std::min(1.0f,128.0f/std::max(width,height));
    image.width = std::max(1,(int)(width*scale)); image.height = std::max(1,(int)(height*scale));
    image.pixels.resize(image.width*image.height*4);
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
        size_t source = ((size_t)(y*height/image.height)*width + x*width/image.width)*4;
        std::memcpy(&image.pixels[(y*image.width+x)*4],pixels.get()+source,4);
    }
    return image;
}
static void UploadPreview(GLuint& texture, const preview_image& image, bool reuse_storage) {
    if (!texture) {
        glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    } else glBindTexture(GL_TEXTURE_2D,texture);
    if (reuse_storage) glTexSubImage2D(GL_TEXTURE_2D,0,0,0,image.width,image.height,GL_RGBA,GL_UNSIGNED_BYTE,image.pixels.data());
    else glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,image.width,image.height,0,GL_RGBA,GL_UNSIGNED_BYTE,image.pixels.data());
    glBindTexture(GL_TEXTURE_2D,0);
}
static void PumpEditorPreviews() {
    for (auto& result : g_editor_previews.take_results()) {
        if (result.key[0] == 'M') {
            int id = std::stoi(result.key.substr(1));
            auto it = g_material_preview_states.find(id);
            if (it == g_material_preview_states.end() || it->second.requested != result.revision || id >= (int)g_scene.materials.size()) continue;
            auto& state = it->second;
            if (!result.image.pixels.empty()) {
                auto& texture = g_material_thumb_cache[id];
                UploadPreview(texture,result.image,texture && state.size == result.image.width);
                state.size = result.image.width;
            }
            state.displayed = result.revision;
        } else {
            auto path = result.key.substr(1);
            auto it = g_texture_preview_requests.find(path);
            if (it == g_texture_preview_requests.end() || it->second != result.revision) continue;
            GLuint texture = 0;
            if (!result.image.pixels.empty()) UploadPreview(texture,result.image,false);
            g_texture_thumb_cache[path] = texture; // Also cache failures; don't retry a missing file every frame.
        }
    }
}
static GLuint GetOrCreateTextureThumbnail(const std::string& path) {
    PumpEditorPreviews();
    auto found = g_texture_thumb_cache.find(path);
    if (found != g_texture_thumb_cache.end()) return found->second;
    if (g_texture_preview_requests.count(path) || g_thumbs_created_this_frame >= g_thumb_budget_per_frame) return 0;
    uint64_t revision = ++g_preview_revision;
    if (g_editor_previews.request("T"+path,revision,[path] { return DecodeTextureThumbnail(path); })) {
        g_texture_preview_requests[path] = revision; ++g_thumbs_created_this_frame;
    }
    return 0;
}
static GLuint GetOrCreateMaterialThumbnail(int index) {
    PumpEditorPreviews();
    if (index < 0 || index >= (int)g_scene.materials.size()) return 0;
    auto& state = g_material_preview_states[index]; auto& texture = g_material_thumb_cache[index];
    if (!texture) {
        preview_image placeholder; placeholder.width = placeholder.height = 1; placeholder.pixels = {37,48,61,255};
        UploadPreview(texture,placeholder,false); state.size = 1; state.dirty = true;
    }
    bool interactive = ImGui::IsAnyItemActive();
    int size = interactive ? 64 : MATERIAL_THUMB_SIZE;
    auto now = std::chrono::steady_clock::now();
    bool refine = !interactive && state.requested_size == 64;
    bool idle = state.requested == state.displayed;
    if ((state.dirty || refine) && idle && (state.requested == 0 || now-state.submitted >= std::chrono::milliseconds(80))) {
        scene assets; assets.textures = g_scene.textures; // Never copy geometry or touch the live scene on the worker.
        auto description = g_scene.materials[index]; uint64_t revision = ++g_preview_revision;
        if (g_editor_previews.request("M"+std::to_string(index),revision,
            [assets = std::move(assets),description = std::move(description),size] { return RenderMaterialPreview(assets,description,size); },index == g_selected_material)) {
            state.requested = revision; state.requested_size = size; state.submitted = now; state.dirty = false;
        }
    }
    return texture;
}
static void InvalidateMaterialThumbnail(int index) {
    // ImGui draw commands may already reference this GL name in this frame.
    // Retain it and the last image until the worker's replacement is ready.
    g_material_preview_states[index].dirty = true;
}
static void InvalidateAllMaterialThumbnails() {
    // Index-changing insert/erase/load operations invalidate outstanding jobs as
    // well as cached images, so an old result can't paint a different material.
    for (auto& item : g_material_preview_states) {
        item.second.requested = item.second.displayed = ++g_preview_revision;
        item.second.dirty = true; item.second.submitted = {};
    }
}
