// SPDX-License-Identifier: MIT
// gui/gui_spaces.cpp — 3D space visualization panel with world-space layer previews.

#include "gui_spaces.h"

#include "gui_common.h"
#include "gui_preview_cache.h"
#include "../instance_data.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL_opengl.h>

namespace debug_layer {

namespace {

constexpr float kViewFrustumNear = 0.01f;
constexpr float kViewFrustumFar = 0.20f;
constexpr float kProjectionPreviewDistance = 0.19f;
constexpr uint64_t kCompositionLayerRetentionFrames = 6;

static struct {
    bool loaded = false;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLACTIVETEXTUREPROC ActiveTexture;
} gl;

static void load_gl_functions()
{
    if (gl.loaded)
        return;
#define LOAD(name) gl.name = (decltype(gl.name))SDL_GL_GetProcAddress("gl" #name)
    LOAD(GenFramebuffers);
    LOAD(BindFramebuffer);
    LOAD(FramebufferTexture2D);
    LOAD(DeleteFramebuffers);
    LOAD(CheckFramebufferStatus);
    LOAD(GenRenderbuffers);
    LOAD(BindRenderbuffer);
    LOAD(RenderbufferStorage);
    LOAD(FramebufferRenderbuffer);
    LOAD(DeleteRenderbuffers);
    LOAD(GenBuffers);
    LOAD(BindBuffer);
    LOAD(BufferData);
    LOAD(DeleteBuffers);
    LOAD(CreateShader);
    LOAD(ShaderSource);
    LOAD(CompileShader);
    LOAD(CreateProgram);
    LOAD(AttachShader);
    LOAD(LinkProgram);
    LOAD(UseProgram);
    LOAD(DeleteShader);
    LOAD(DeleteProgram);
    LOAD(GetUniformLocation);
    LOAD(UniformMatrix4fv);
    LOAD(Uniform1i);
    LOAD(GenVertexArrays);
    LOAD(BindVertexArray);
    LOAD(DeleteVertexArrays);
    LOAD(EnableVertexAttribArray);
    LOAD(VertexAttribPointer);
    LOAD(ActiveTexture);
#undef LOAD
    gl.loaded = true;
}

struct LineVertex {
    float x, y, z;
    float r, g, b, a;
};

struct TexturedVertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
};

struct TexturedQuad {
    GLuint texture = 0;
    std::array<TexturedVertex, 6> vertices = {};
};

struct LabelInfo {
    Vec3 pos;
    std::string text;
    ImVec4 color;
};

struct DisplayedLayer {
    const TrackedCompositionLayer *layer = nullptr;
    uint64_t source_frame_number = 0;
    size_t source_layer_index = 0;
    uint64_t stale_frame_count = 0;
};

struct HoverLayerInfo {
    std::array<Vec3, 4> world_corners = {};
    std::string overlay_text;
    ImVec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    std::string type_label;
    std::string space_label;
    uint64_t source_frame_number = 0;
    uint64_t stale_frame_count = 0;
    XrCompositionLayerFlags layer_flags = 0;
    XrEyeVisibility eye_visibility = XR_EYE_VISIBILITY_BOTH;
    bool has_pose = false;
    XrPosef pose = {{0, 0, 0, 1}, {0, 0, 0}};
    bool has_fov = false;
    XrFovf fov = {0, 0, 0, 0};
    bool has_size = false;
    XrExtent2Df size = {0.0f, 0.0f};
    TrackedCompositionSubImage sub_image;
    bool has_swapchain = false;
    uint64_t swapchain_handle = 0;
    uint32_t swapchain_width = 0;
    uint32_t swapchain_height = 0;
    int64_t swapchain_format = 0;
    uint32_t swapchain_sample_count = 0;
    uint32_t swapchain_mip_count = 0;
    bool has_preview = false;
    uint32_t preview_width = 0;
    uint32_t preview_height = 0;
    uint32_t preview_array_index = 0;
    uint64_t preview_serial = 0;
};

struct ScreenHoverLayer {
    const HoverLayerInfo *info = nullptr;
    std::array<ImVec2, 4> corners = {};
    ImVec2 center = {};
    ImU32 color = 0;
};

struct SceneStats {
    size_t textured_layer_count = 0;
    size_t outlined_layer_count = 0;
    size_t unresolved_layer_count = 0;
};

static struct SceneState {
    GLuint fbo = 0;
    GLuint fbo_texture = 0;
    GLuint depth_rbo = 0;
    int fbo_width = 0;
    int fbo_height = 0;
    GLuint line_shader_program = 0;
    GLint line_mvp_loc = -1;
    GLuint textured_shader_program = 0;
    GLint textured_mvp_loc = -1;
    GLint textured_sampler_loc = -1;
    GLuint line_vao = 0;
    GLuint line_vbo = 0;
    GLuint textured_vao = 0;
    GLuint textured_vbo = 0;
    ArcballCamera camera;
} scene;

static void ensure_fbo(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    if (scene.fbo != 0 && scene.fbo_width == width && scene.fbo_height == height)
        return;

    if (scene.fbo != 0) {
        gl.DeleteFramebuffers(1, &scene.fbo);
        glDeleteTextures(1, &scene.fbo_texture);
        gl.DeleteRenderbuffers(1, &scene.depth_rbo);
    }

    scene.fbo_width = width;
    scene.fbo_height = height;

    glGenTextures(1, &scene.fbo_texture);
    glBindTexture(GL_TEXTURE_2D, scene.fbo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    gl.GenRenderbuffers(1, &scene.depth_rbo);
    gl.BindRenderbuffer(GL_RENDERBUFFER, scene.depth_rbo);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    gl.BindRenderbuffer(GL_RENDERBUFFER, 0);

    gl.GenFramebuffers(1, &scene.fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            scene.fbo_texture, 0);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_RENDERBUFFER, scene.depth_rbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

static GLuint compile_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = gl.CreateShader(GL_VERTEX_SHADER);
    gl.ShaderSource(vs, 1, &vs_src, nullptr);
    gl.CompileShader(vs);

    GLuint fs = gl.CreateShader(GL_FRAGMENT_SHADER);
    gl.ShaderSource(fs, 1, &fs_src, nullptr);
    gl.CompileShader(fs);

    GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.LinkProgram(program);

    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    return program;
}

static void ensure_line_shader()
{
    if (scene.line_shader_program != 0)
        return;

    const char *vs_src = R"(
        #version 330 core
        layout(location = 0) in vec3 aPos;
        layout(location = 1) in vec4 aColor;
        uniform mat4 uMVP;
        out vec4 vColor;
        void main() {
            gl_Position = uMVP * vec4(aPos, 1.0);
            vColor = aColor;
        }
    )";

    const char *fs_src = R"(
        #version 330 core
        in vec4 vColor;
        out vec4 FragColor;
        void main() {
            FragColor = vColor;
        }
    )";

    scene.line_shader_program = compile_program(vs_src, fs_src);
    scene.line_mvp_loc = gl.GetUniformLocation(scene.line_shader_program, "uMVP");
}

static void ensure_textured_shader()
{
    if (scene.textured_shader_program != 0)
        return;

    const char *vs_src = R"(
        #version 330 core
        layout(location = 0) in vec3 aPos;
        layout(location = 1) in vec2 aUV;
        layout(location = 2) in vec4 aColor;
        uniform mat4 uMVP;
        out vec2 vUV;
        out vec4 vColor;
        void main() {
            gl_Position = uMVP * vec4(aPos, 1.0);
            vUV = aUV;
            vColor = aColor;
        }
    )";

    const char *fs_src = R"(
        #version 330 core
        in vec2 vUV;
        in vec4 vColor;
        uniform sampler2D uTexture;
        out vec4 FragColor;
        void main() {
            FragColor = texture(uTexture, vUV) * vColor;
        }
    )";

    scene.textured_shader_program = compile_program(vs_src, fs_src);
    scene.textured_mvp_loc =
        gl.GetUniformLocation(scene.textured_shader_program, "uMVP");
    scene.textured_sampler_loc =
        gl.GetUniformLocation(scene.textured_shader_program, "uTexture");
}

static void ensure_line_vao_vbo()
{
    if (scene.line_vao != 0)
        return;

    gl.GenVertexArrays(1, &scene.line_vao);
    gl.GenBuffers(1, &scene.line_vbo);

    gl.BindVertexArray(scene.line_vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, scene.line_vbo);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), (void *)0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                           (void *)(3 * sizeof(float)));
    gl.BindVertexArray(0);
}

static void ensure_textured_vao_vbo()
{
    if (scene.textured_vao != 0)
        return;

    gl.GenVertexArrays(1, &scene.textured_vao);
    gl.GenBuffers(1, &scene.textured_vbo);

    gl.BindVertexArray(scene.textured_vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, scene.textured_vbo);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TexturedVertex),
                           (void *)0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TexturedVertex),
                           (void *)(3 * sizeof(float)));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(TexturedVertex),
                           (void *)(5 * sizeof(float)));
    gl.BindVertexArray(0);
}

static void add_line(std::vector<LineVertex> &verts, Vec3 a, Vec3 b, float r, float g,
                     float bl, float alpha = 1.0f)
{
    verts.push_back({a.x, a.y, a.z, r, g, bl, alpha});
    verts.push_back({b.x, b.y, b.z, r, g, bl, alpha});
}

static void add_grid(std::vector<LineVertex> &verts, float extent, float spacing)
{
    float gray = 0.25f;
    for (float i = -extent; i <= extent; i += spacing) {
        add_line(verts, {i, 0, -extent}, {i, 0, extent}, gray, gray, gray, 0.5f);
        add_line(verts, {-extent, 0, i}, {extent, 0, i}, gray, gray, gray, 0.5f);
    }
}

static void add_axes(std::vector<LineVertex> &verts, Vec3 origin, float length,
                     float alpha = 1.0f)
{
    add_line(verts, origin, origin + Vec3{length, 0, 0}, 1.0f, 0.2f, 0.2f, alpha);
    add_line(verts, origin, origin + Vec3{0, length, 0}, 0.2f, 1.0f, 0.2f, alpha);
    add_line(verts, origin, origin + Vec3{0, 0, length}, 0.2f, 0.2f, 1.0f, alpha);
}

static Mat4 pose_to_mat(const XrPosef &pose)
{
    return Mat4::from_quat_pos(pose.orientation.x, pose.orientation.y,
                               pose.orientation.z, pose.orientation.w,
                               pose.position.x, pose.position.y, pose.position.z);
}

static Vec3 transform_point(const Mat4 &m, float x, float y, float z)
{
    Vec4 world = m * Vec4{x, y, z, 1.0f};
    return {world.x, world.y, world.z};
}

static Vec3 mat_translation(const Mat4 &m)
{
    return {m.m[12], m.m[13], m.m[14]};
}

static void add_coordinate_frame(std::vector<LineVertex> &verts, const Mat4 &transform,
                                 float size, float alpha = 1.0f)
{
    Vec3 origin = mat_translation(transform);
    Vec4 rx = transform * Vec4{size, 0, 0, 0};
    Vec4 ry = transform * Vec4{0, size, 0, 0};
    Vec4 rz = transform * Vec4{0, 0, size, 0};

    add_line(verts, origin, origin + Vec3{rx.x, rx.y, rx.z}, 1.0f, 0.2f, 0.2f, alpha);
    add_line(verts, origin, origin + Vec3{ry.x, ry.y, ry.z}, 0.2f, 1.0f, 0.2f, alpha);
    add_line(verts, origin, origin + Vec3{rz.x, rz.y, rz.z}, 0.2f, 0.2f, 1.0f, alpha);
}

static void compute_frustum_corners(const Mat4 &transform, const XrFovf &fov, float dist,
                                    Vec3 corners[4])
{
    float tan_left = std::tan(fov.angleLeft);
    float tan_right = std::tan(fov.angleRight);
    float tan_up = std::tan(fov.angleUp);
    float tan_down = std::tan(fov.angleDown);

    float l = tan_left * dist;
    float r = tan_right * dist;
    float u = tan_up * dist;
    float d = tan_down * dist;

    corners[0] = transform_point(transform, l, u, -dist);
    corners[1] = transform_point(transform, r, u, -dist);
    corners[2] = transform_point(transform, r, d, -dist);
    corners[3] = transform_point(transform, l, d, -dist);
}

static void add_fov_frustum(std::vector<LineVertex> &verts, const Mat4 &transform,
                            const XrFovf &fov, float near_dist, float far_dist,
                            float r, float g, float b, float alpha)
{
    Vec3 origin = mat_translation(transform);
    Vec3 near_corners[4], far_corners[4];
    compute_frustum_corners(transform, fov, near_dist, near_corners);
    compute_frustum_corners(transform, fov, far_dist, far_corners);

    for (int i = 0; i < 4; i++)
        add_line(verts, origin, far_corners[i], r, g, b, alpha * 0.35f);
    for (int i = 0; i < 4; i++)
        add_line(verts, near_corners[i], near_corners[(i + 1) % 4], r, g, b, alpha);
    for (int i = 0; i < 4; i++)
        add_line(verts, far_corners[i], far_corners[(i + 1) % 4], r, g, b, alpha * 0.55f);
}

static void add_quad_outline(std::vector<LineVertex> &verts, const std::array<Vec3, 4> &corners,
                             float r, float g, float b, float alpha)
{
    for (int i = 0; i < 4; i++)
        add_line(verts, corners[i], corners[(i + 1) % 4], r, g, b, alpha);
}

static std::array<Vec3, 4> oriented_quad_corners(const Mat4 &transform, float width,
                                                 float height)
{
    float half_w = width * 0.5f;
    float half_h = height * 0.5f;
    return {transform_point(transform, -half_w, half_h, 0.0f),
            transform_point(transform, half_w, half_h, 0.0f),
            transform_point(transform, half_w, -half_h, 0.0f),
            transform_point(transform, -half_w, -half_h, 0.0f)};
}

static void add_textured_quad(std::vector<TexturedQuad> &quads, GLuint texture,
                              const std::array<Vec3, 4> &corners, float u0, float v0,
                              float u1, float v1, float r, float g, float b,
                              float alpha)
{
    TexturedQuad quad;
    quad.texture = texture;
    quad.vertices = {{{corners[0].x, corners[0].y, corners[0].z, u0, v0, r, g, b, alpha},
                      {corners[1].x, corners[1].y, corners[1].z, u1, v0, r, g, b, alpha},
                      {corners[2].x, corners[2].y, corners[2].z, u1, v1, r, g, b, alpha},
                      {corners[0].x, corners[0].y, corners[0].z, u0, v0, r, g, b, alpha},
                      {corners[2].x, corners[2].y, corners[2].z, u1, v1, r, g, b, alpha},
                      {corners[3].x, corners[3].y, corners[3].z, u0, v1, r, g, b, alpha}}};
    quads.push_back(quad);
}

static ImVec4 color_for_space(const TrackedSpace &sp)
{
    if (sp.kind == SpaceKind::REFERENCE) {
        switch (sp.reference_type) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: return {1.0f, 0.6f, 0.2f, 1.0f};
        case XR_REFERENCE_SPACE_TYPE_LOCAL: return {0.3f, 0.5f, 1.0f, 1.0f};
        case XR_REFERENCE_SPACE_TYPE_STAGE: return {0.3f, 1.0f, 0.3f, 1.0f};
        default: return {0.8f, 0.8f, 0.8f, 1.0f};
        }
    }
    return {1.0f, 1.0f, 0.3f, 1.0f};
}

static const char *layer_type_to_str(XrStructureType type)
{
    switch (type) {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: return "Projection";
    case XR_TYPE_COMPOSITION_LAYER_QUAD: return "Quad";
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: return "Cylinder";
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: return "Equirect";
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: return "Equirect2";
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: return "Cube";
    default: return "Unknown";
    }
}

static const char *eye_visibility_to_str(XrEyeVisibility eye_visibility)
{
    switch (eye_visibility) {
    case XR_EYE_VISIBILITY_BOTH: return "BOTH";
    case XR_EYE_VISIBILITY_LEFT: return "LEFT";
    case XR_EYE_VISIBILITY_RIGHT: return "RIGHT";
    default: return "UNKNOWN";
    }
}

static std::string format_vec3_text(const Vec3 &pos)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
    return buffer;
}

static std::string space_label_for_hover(const std::unordered_map<XrSpace, TrackedSpace> &spaces,
                                         XrSpace space)
{
    if (space == XR_NULL_HANDLE)
        return "(none)";

    auto it = spaces.find(space);
    if (it != spaces.end())
        return it->second.label();

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", (unsigned long long)space);
    return buffer;
}

static bool has_valid_pose(const XrSpaceLocation &location)
{
    XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    return (location.locationFlags & required) == required;
}

static bool resolve_space_transform(XrSpace space, XrSpace scene_root,
                                    const std::unordered_map<XrSpace, TrackedSpace> &spaces,
                                    std::unordered_set<XrSpace> &visiting,
                                    Mat4 &out_transform)
{
    if (space == scene_root || space == XR_NULL_HANDLE) {
        out_transform = Mat4::identity();
        return space == scene_root || scene_root == XR_NULL_HANDLE;
    }

    if (!visiting.insert(space).second)
        return false;

    auto it = spaces.find(space);
    if (it == spaces.end() || !it->second.has_location || !has_valid_pose(it->second.latest_location)) {
        visiting.erase(space);
        return false;
    }

    Mat4 base_transform;
    XrSpace base_space = it->second.located_relative_to;
    bool resolved = false;
    if (base_space == scene_root) {
        base_transform = Mat4::identity();
        resolved = true;
    } else {
        resolved = resolve_space_transform(base_space, scene_root, spaces, visiting,
                                           base_transform);
    }

    visiting.erase(space);
    if (!resolved)
        return false;

    out_transform = base_transform * pose_to_mat(it->second.latest_location.pose);
    return true;
}

static bool resolve_view_transform(const TrackedViewPose &view_pose, XrSpace scene_root,
                                   const std::unordered_map<XrSpace, TrackedSpace> &spaces,
                                   Mat4 &out_transform)
{
    Mat4 base_transform = Mat4::identity();
    if (view_pose.base_space != XR_NULL_HANDLE && view_pose.base_space != scene_root) {
        std::unordered_set<XrSpace> visiting;
        if (!resolve_space_transform(view_pose.base_space, scene_root, spaces, visiting,
                                     base_transform))
            return false;
    }

    out_transform = base_transform * pose_to_mat(view_pose.pose);
    return true;
}

static XrSpace choose_scene_root(const std::unordered_map<XrSpace, TrackedSpace> &spaces,
                                 const std::vector<TrackedViewPose> &view_poses)
{
    for (const TrackedViewPose &vp : view_poses) {
        if (vp.valid && vp.base_space != XR_NULL_HANDLE)
            return vp.base_space;
    }

    const XrReferenceSpaceType preferred_roots[] = {
        XR_REFERENCE_SPACE_TYPE_LOCAL,
        XR_REFERENCE_SPACE_TYPE_STAGE,
        XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
        XR_REFERENCE_SPACE_TYPE_VIEW,
    };

    for (XrReferenceSpaceType preferred : preferred_roots) {
        for (const auto &[handle, space] : spaces) {
            if (space.kind == SpaceKind::REFERENCE && space.reference_type == preferred)
                return handle;
        }
    }

    return XR_NULL_HANDLE;
}

static std::string describe_scene_root(const std::unordered_map<XrSpace, TrackedSpace> &spaces,
                                       XrSpace root)
{
    if (root == XR_NULL_HANDLE)
        return "(unresolved)";

    auto it = spaces.find(root);
    if (it != spaces.end())
        return it->second.label();

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", (unsigned long long)root);
    return buffer;
}

static std::string layer_identity_key(size_t layer_index, const TrackedCompositionLayer &layer)
{
    std::string key = std::to_string(layer_index) + ':' +
                      std::to_string((int)layer.type) + ':' +
                      std::to_string((uint64_t)layer.space) + ':' +
                      std::to_string((int)layer.eye_visibility);
    if (layer.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        key += ":views=" + std::to_string(layer.projection_views.size());
        for (const auto &view : layer.projection_views) {
            key += ':' + std::to_string((uint64_t)view.sub_image.swapchain) + ':' +
                   std::to_string(view.sub_image.image_array_index) + ':' +
                   std::to_string(view.sub_image.offset_x) + ':' +
                   std::to_string(view.sub_image.offset_y) + ':' +
                   std::to_string(view.sub_image.extent_width) + ':' +
                   std::to_string(view.sub_image.extent_height);
        }
    } else {
        key += ':' + std::to_string((uint64_t)layer.sub_image.swapchain) + ':' +
               std::to_string(layer.sub_image.image_array_index) + ':' +
               std::to_string(layer.sub_image.offset_x) + ':' +
               std::to_string(layer.sub_image.offset_y) + ':' +
               std::to_string(layer.sub_image.extent_width) + ':' +
               std::to_string(layer.sub_image.extent_height);
    }
    return key;
}

static std::vector<DisplayedLayer> collect_display_layers(
    const std::deque<TrackedCompositionFrame> &frames)
{
    std::vector<DisplayedLayer> layers;
    if (frames.empty())
        return layers;

    uint64_t newest_frame_number = frames.back().frame_number;
    std::unordered_set<std::string> seen_keys;
    for (auto frame_it = frames.rbegin(); frame_it != frames.rend(); ++frame_it) {
        uint64_t stale_frame_count = newest_frame_number >= frame_it->frame_number
                                         ? newest_frame_number - frame_it->frame_number
                                         : 0;
        if (stale_frame_count > kCompositionLayerRetentionFrames)
            break;

        for (size_t layer_index = 0; layer_index < frame_it->layers.size(); ++layer_index) {
            const TrackedCompositionLayer &layer = frame_it->layers[layer_index];
            std::string key = layer_identity_key(layer_index, layer);
            if (!seen_keys.insert(key).second)
                continue;

            layers.push_back({&layer, frame_it->frame_number, layer_index, stale_frame_count});
        }
    }

    return layers;
}

static bool compute_sub_image_uv_rect(const TrackedSwapchain &swapchain,
                                      const TrackedCompositionSubImage &sub_image,
                                      float &u0, float &v0, float &u1, float &v1)
{
    if (swapchain.width == 0 || swapchain.height == 0)
        return false;

    int32_t rect_left = sub_image.offset_x;
    int32_t rect_top = sub_image.offset_y;
    int32_t rect_right = rect_left + std::max(sub_image.extent_width, 1);
    int32_t rect_bottom = rect_top + std::max(sub_image.extent_height, 1);
    rect_left = std::clamp(rect_left, 0, (int32_t)swapchain.width);
    rect_top = std::clamp(rect_top, 0, (int32_t)swapchain.height);
    rect_right = std::clamp(rect_right, 0, (int32_t)swapchain.width);
    rect_bottom = std::clamp(rect_bottom, 0, (int32_t)swapchain.height);
    if (rect_left == rect_right || rect_top == rect_bottom)
        return false;

    u0 = (float)rect_left / (float)swapchain.width;
    v0 = (float)rect_top / (float)swapchain.height;
    u1 = (float)rect_right / (float)swapchain.width;
    v1 = (float)rect_bottom / (float)swapchain.height;
    return true;
}

static bool project_quad_to_screen(const Mat4 &mvp, const std::array<Vec3, 4> &world_corners,
                                   float viewport_w, float viewport_h, const ImVec2 &origin,
                                   std::array<ImVec2, 4> &screen_corners, ImVec2 &center)
{
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    for (size_t i = 0; i < world_corners.size(); ++i) {
        float sx = 0.0f;
        float sy = 0.0f;
        if (!project_to_screen(mvp, world_corners[i], viewport_w, viewport_h, sx, sy))
            return false;
        screen_corners[i] = ImVec2(origin.x + sx, origin.y + sy);
        sum_x += screen_corners[i].x;
        sum_y += screen_corners[i].y;
    }

    center = ImVec2(sum_x / 4.0f, sum_y / 4.0f);
    return true;
}

static float signed_area_2d(const ImVec2 &a, const ImVec2 &b, const ImVec2 &c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool point_in_triangle_2d(const ImVec2 &point, const ImVec2 &a, const ImVec2 &b,
                                 const ImVec2 &c)
{
    float area1 = signed_area_2d(point, a, b);
    float area2 = signed_area_2d(point, b, c);
    float area3 = signed_area_2d(point, c, a);
    bool has_negative = area1 < 0.0f || area2 < 0.0f || area3 < 0.0f;
    bool has_positive = area1 > 0.0f || area2 > 0.0f || area3 > 0.0f;
    return !(has_negative && has_positive);
}

static bool point_in_quad_2d(const ImVec2 &point, const std::array<ImVec2, 4> &quad)
{
    return point_in_triangle_2d(point, quad[0], quad[1], quad[2]) ||
           point_in_triangle_2d(point, quad[0], quad[2], quad[3]);
}

static void render_hover_layer_tooltip(const HoverLayerInfo &info)
{
    ImGui::BeginTooltip();
    ImGui::Text("Type: %s", info.type_label.c_str());
    ImGui::Text("Space: %s", info.space_label.c_str());
    ImGui::Text("Layer flags: %#llx", (unsigned long long)info.layer_flags);
    ImGui::Text("Eye visibility: %s", eye_visibility_to_str(info.eye_visibility));
    ImGui::Text("Last seen in frame: %llu", (unsigned long long)info.source_frame_number);
    if (info.stale_frame_count > 0)
        ImGui::TextDisabled("Stale for %llu frame%s",
                            (unsigned long long)info.stale_frame_count,
                            info.stale_frame_count == 1 ? "" : "s");

    if (info.has_pose) {
        ImGui::SeparatorText("Pose");
        ImGui::Text("Position: (%.3f, %.3f, %.3f)", info.pose.position.x,
                    info.pose.position.y, info.pose.position.z);
        ImGui::Text("Orientation: (%.3f, %.3f, %.3f, %.3f)",
                    info.pose.orientation.x, info.pose.orientation.y,
                    info.pose.orientation.z, info.pose.orientation.w);
    }

    if (info.has_fov) {
        ImGui::SeparatorText("FOV");
        ImGui::Text("L=%.3f R=%.3f U=%.3f D=%.3f", info.fov.angleLeft,
                    info.fov.angleRight, info.fov.angleUp, info.fov.angleDown);
    }

    if (info.has_size) {
        ImGui::SeparatorText("Size");
        ImGui::Text("%.3f x %.3f", info.size.width, info.size.height);
    }

    ImGui::SeparatorText("Sub-image");
    ImGui::Text("Swapchain: 0x%llx", (unsigned long long)info.swapchain_handle);
    if (info.sub_image.has_image_index)
        ImGui::Text("Image index: %u", info.sub_image.image_index);
    else
        ImGui::TextDisabled("Image index: unavailable");
    ImGui::Text("Image rect: offset=(%d,%d) extent=(%d,%d)",
                info.sub_image.offset_x, info.sub_image.offset_y,
                info.sub_image.extent_width, info.sub_image.extent_height);
    ImGui::Text("Array index: %u", info.sub_image.image_array_index);

    if (info.has_swapchain) {
        ImGui::Text("Swapchain extent: %ux%u", info.swapchain_width, info.swapchain_height);
        ImGui::Text("Format: %#llx  Samples: %u  Mips: %u",
                    (unsigned long long)info.swapchain_format,
                    info.swapchain_sample_count, info.swapchain_mip_count);
    }

    if (info.sub_image.has_depth) {
        ImGui::SeparatorText("Depth");
        ImGui::Text("Depth swapchain: 0x%llx",
                    (unsigned long long)info.sub_image.depth_swapchain);
        ImGui::Text("Depth rect: offset=(%d,%d) extent=(%d,%d) array=%u",
                    info.sub_image.depth_offset_x, info.sub_image.depth_offset_y,
                    info.sub_image.depth_extent_width, info.sub_image.depth_extent_height,
                    info.sub_image.depth_image_array_index);
        ImGui::Text("Depth range: min=%.3f max=%.3f near=%.3f far=%.3f",
                    info.sub_image.min_depth, info.sub_image.max_depth,
                    info.sub_image.near_z, info.sub_image.far_z);
    }

    if (info.has_preview) {
        ImGui::SeparatorText("Preview");
        ImGui::Text("Preview: %ux%u  serial=%llu  array=%u", info.preview_width,
                    info.preview_height, (unsigned long long)info.preview_serial,
                    info.preview_array_index);
    } else {
        ImGui::TextDisabled("Preview unavailable for this sub-image.");
    }
    ImGui::EndTooltip();
}

static void append_projection_layer_geometry(
    const TrackedCompositionLayer &layer, uint64_t source_frame_number,
    uint64_t stale_frame_count,
    XrSpace scene_root, const std::unordered_map<XrSpace, TrackedSpace> &spaces,
    const std::unordered_map<XrSwapchain, TrackedSwapchain> &swapchains,
    std::vector<LineVertex> &lines, std::vector<TexturedQuad> &quads,
    std::vector<LabelInfo> &labels, std::vector<HoverLayerInfo> &hover_layers,
    SceneStats &stats)
{
    Mat4 layer_space_transform = Mat4::identity();
    if (layer.space != XR_NULL_HANDLE && layer.space != scene_root) {
        std::unordered_set<XrSpace> visiting;
        if (!resolve_space_transform(layer.space, scene_root, spaces, visiting,
                                     layer_space_transform)) {
            stats.unresolved_layer_count++;
            return;
        }
    }

    for (size_t view_index = 0; view_index < layer.projection_views.size(); ++view_index) {
        const TrackedCompositionProjectionView &view = layer.projection_views[view_index];
        Mat4 view_transform = layer_space_transform * pose_to_mat(view.pose);

        add_fov_frustum(lines, view_transform, view.fov, kViewFrustumNear, kViewFrustumFar,
                        0.95f, 0.72f, 0.24f, 0.45f);

        Vec3 corners_buf[4];
        compute_frustum_corners(view_transform, view.fov, kProjectionPreviewDistance,
                                corners_buf);
        std::array<Vec3, 4> plane_corners = {corners_buf[0], corners_buf[1],
                                             corners_buf[2], corners_buf[3]};
        add_quad_outline(lines, plane_corners, 0.95f, 0.72f, 0.24f, 0.85f);
        stats.outlined_layer_count++;

        HoverLayerInfo hover_info;
        hover_info.world_corners = plane_corners;
        hover_info.overlay_text = stale_frame_count == 0
                                      ? "Proj " + std::to_string(view_index)
                                      : "Proj " + std::to_string(view_index) + " stale";
        hover_info.color = {0.95f, 0.72f, 0.24f, 1.0f};
        hover_info.type_label = layer_type_to_str(layer.type);
        hover_info.space_label = space_label_for_hover(spaces, layer.space);
        hover_info.source_frame_number = source_frame_number;
        hover_info.stale_frame_count = stale_frame_count;
        hover_info.layer_flags = layer.layer_flags;
        hover_info.eye_visibility = layer.eye_visibility;
        hover_info.has_pose = true;
        hover_info.pose = view.pose;
        hover_info.has_fov = true;
        hover_info.fov = view.fov;
        hover_info.sub_image = view.sub_image;

        auto swapchain_it = swapchains.find(view.sub_image.swapchain);
        if (swapchain_it != swapchains.end()) {
            hover_info.has_swapchain = true;
            hover_info.swapchain_handle = (uint64_t)swapchain_it->first;
            hover_info.swapchain_width = swapchain_it->second.width;
            hover_info.swapchain_height = swapchain_it->second.height;
            hover_info.swapchain_format = swapchain_it->second.format;
            hover_info.swapchain_sample_count = swapchain_it->second.sample_count;
            hover_info.swapchain_mip_count = swapchain_it->second.mip_count;
            PreviewLookupResult preview_lookup =
                gui_lookup_preview_for_sub_image(swapchain_it->second, view.sub_image);
            if (preview_lookup.preview != nullptr && preview_lookup.preview->available) {
                hover_info.has_preview = true;
                hover_info.preview_width = preview_lookup.preview->width;
                hover_info.preview_height = preview_lookup.preview->height;
                hover_info.preview_array_index = preview_lookup.preview->image_array_index;
                hover_info.preview_serial = preview_lookup.preview->capture_serial;
                GLuint texture =
                    gui_ensure_preview_texture(preview_lookup.texture_key, *preview_lookup.preview);
                float u0, v0, u1, v1;
                if (compute_sub_image_uv_rect(swapchain_it->second, view.sub_image, u0, v0,
                                              u1, v1)) {
                    float alpha = stale_frame_count == 0 ? 0.92f : 0.60f;
                    add_textured_quad(quads, texture, plane_corners, u0, v0, u1, v1,
                                      1.0f, 1.0f, 1.0f, alpha);
                    stats.textured_layer_count++;
                }
            }
        }

        hover_layers.push_back(std::move(hover_info));
    }
}

static void append_quad_layer_geometry(
    const TrackedCompositionLayer &layer, uint64_t source_frame_number,
    uint64_t stale_frame_count,
    XrSpace scene_root, const std::unordered_map<XrSpace, TrackedSpace> &spaces,
    const std::unordered_map<XrSwapchain, TrackedSwapchain> &swapchains,
    std::vector<LineVertex> &lines, std::vector<TexturedQuad> &quads,
    std::vector<LabelInfo> &labels, std::vector<HoverLayerInfo> &hover_layers,
    SceneStats &stats)
{
    Mat4 layer_space_transform = Mat4::identity();
    if (layer.space != XR_NULL_HANDLE && layer.space != scene_root) {
        std::unordered_set<XrSpace> visiting;
        if (!resolve_space_transform(layer.space, scene_root, spaces, visiting,
                                     layer_space_transform)) {
            stats.unresolved_layer_count++;
            return;
        }
    }

    Mat4 quad_transform = layer_space_transform * pose_to_mat(layer.pose);
    std::array<Vec3, 4> corners =
        oriented_quad_corners(quad_transform, layer.size.width, layer.size.height);
    add_quad_outline(lines, corners, 0.28f, 0.86f, 1.0f, 0.9f);
    stats.outlined_layer_count++;

    HoverLayerInfo hover_info;
    hover_info.world_corners = corners;
    hover_info.overlay_text = std::string("Quad ") + format_vec3_text(mat_translation(quad_transform));
    hover_info.color = {0.28f, 0.86f, 1.0f, 1.0f};
    hover_info.type_label = layer_type_to_str(layer.type);
    hover_info.space_label = space_label_for_hover(spaces, layer.space);
    hover_info.source_frame_number = source_frame_number;
    hover_info.stale_frame_count = stale_frame_count;
    hover_info.layer_flags = layer.layer_flags;
    hover_info.eye_visibility = layer.eye_visibility;
    hover_info.has_pose = true;
    hover_info.pose = layer.pose;
    hover_info.has_size = true;
    hover_info.size = layer.size;
    hover_info.sub_image = layer.sub_image;

    auto swapchain_it = swapchains.find(layer.sub_image.swapchain);
    if (swapchain_it != swapchains.end()) {
        hover_info.has_swapchain = true;
        hover_info.swapchain_handle = (uint64_t)swapchain_it->first;
        hover_info.swapchain_width = swapchain_it->second.width;
        hover_info.swapchain_height = swapchain_it->second.height;
        hover_info.swapchain_format = swapchain_it->second.format;
        hover_info.swapchain_sample_count = swapchain_it->second.sample_count;
        hover_info.swapchain_mip_count = swapchain_it->second.mip_count;
        PreviewLookupResult preview_lookup =
            gui_lookup_preview_for_sub_image(swapchain_it->second, layer.sub_image);
        if (preview_lookup.preview != nullptr && preview_lookup.preview->available) {
            hover_info.has_preview = true;
            hover_info.preview_width = preview_lookup.preview->width;
            hover_info.preview_height = preview_lookup.preview->height;
            hover_info.preview_array_index = preview_lookup.preview->image_array_index;
            hover_info.preview_serial = preview_lookup.preview->capture_serial;
            GLuint texture =
                gui_ensure_preview_texture(preview_lookup.texture_key, *preview_lookup.preview);
            float u0, v0, u1, v1;
            if (compute_sub_image_uv_rect(swapchain_it->second, layer.sub_image, u0, v0,
                                          u1, v1)) {
                float alpha = stale_frame_count == 0 ? 0.95f : 0.65f;
                add_textured_quad(quads, texture, corners, u0, v0, u1, v1,
                                  1.0f, 1.0f, 1.0f, alpha);
                stats.textured_layer_count++;
            }
        }
    }

    hover_layers.push_back(std::move(hover_info));
}

} // namespace

void gui_render_spaces_panel(InstanceData *data)
{
    load_gl_functions();

    ImGui::Begin("3D Spaces");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = std::max(10, (int)avail.x);
    int h = std::max(10, (int)avail.y);

    ensure_fbo(w, h);
    ensure_line_shader();
    ensure_textured_shader();
    ensure_line_vao_vbo();
    ensure_textured_vao_vbo();

    if (scene.fbo == 0 || scene.line_shader_program == 0 || scene.textured_shader_program == 0) {
        ImGui::TextDisabled("GL resources not available.");
        ImGui::End();
        return;
    }

    std::vector<LineVertex> lines;
    std::vector<TexturedQuad> quads;
    std::vector<LabelInfo> labels;
    std::vector<HoverLayerInfo> hover_layers;
    SceneStats stats;
    std::string root_label;

    add_grid(lines, 5.0f, 1.0f);
    add_axes(lines, {0, 0, 0}, 0.5f);

    {
        std::shared_lock lock(data->state_mutex);
        XrSpace scene_root = choose_scene_root(data->spaces, data->view_poses);
        root_label = describe_scene_root(data->spaces, scene_root);

        for (const auto &[handle, sp] : data->spaces) {
            if (!sp.has_location || !has_valid_pose(sp.latest_location))
                continue;

            Mat4 transform;
            if (handle == scene_root) {
                transform = Mat4::identity();
            } else {
                std::unordered_set<XrSpace> visiting;
                if (!resolve_space_transform(handle, scene_root, data->spaces, visiting,
                                             transform))
                    continue;
            }

            float alpha = 1.0f;
            if (sp.kind == SpaceKind::ACTION) {
                ActionStateKey key{sp.action, sp.subaction_path};
                auto sit = data->action_states.find(key);
                if (sit != data->action_states.end() && !sit->second.is_active)
                    alpha = 0.3f;
            }

            add_coordinate_frame(lines, transform,
                                 sp.kind == SpaceKind::REFERENCE ? 0.3f : 0.2f, alpha);
            labels.push_back({mat_translation(transform), sp.label(), color_for_space(sp)});
        }

        for (const TrackedViewPose &vp : data->view_poses) {
            if (!vp.valid)
                continue;

            Mat4 transform;
            if (!resolve_view_transform(vp, scene_root, data->spaces, transform))
                continue;

            add_coordinate_frame(lines, transform, 0.15f, 1.0f);
            add_fov_frustum(lines, transform, vp.fov, kViewFrustumNear, kViewFrustumFar,
                            0.9f, 0.7f, 0.2f, 0.5f);
            labels.push_back({mat_translation(transform), vp.label, {1.0f, 0.7f, 0.2f, 1.0f}});
        }

        for (const DisplayedLayer &displayed_layer : collect_display_layers(data->composition_frames)) {
            const TrackedCompositionLayer &layer = *displayed_layer.layer;
            switch (layer.type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
                append_projection_layer_geometry(layer, displayed_layer.source_frame_number,
                                                 displayed_layer.stale_frame_count,
                                                 scene_root, data->spaces, data->swapchains,
                                                 lines, quads, labels, hover_layers, stats);
                break;
            case XR_TYPE_COMPOSITION_LAYER_QUAD:
                append_quad_layer_geometry(layer, displayed_layer.source_frame_number,
                                           displayed_layer.stale_frame_count,
                                           scene_root, data->spaces, data->swapchains,
                                           lines, quads, labels, hover_layers, stats);
                break;
            default:
                break;
            }
        }
    }

    ImGui::TextDisabled(
        "Scene root: %s  |  Textured layers: %zu  |  Outlines: %zu  |  Unresolved: %zu",
        root_label.c_str(), stats.textured_layer_count, stats.outlined_layer_count,
        stats.unresolved_layer_count);

    gl.BindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    float aspect = (float)w / (float)h;
    Mat4 proj = Mat4::perspective(0.9f, aspect, 0.1f, 100.0f);
    Mat4 view = scene.camera.get_view();
    Mat4 mvp = proj * view;

    if (!quads.empty()) {
        gl.UseProgram(scene.textured_shader_program);
        gl.UniformMatrix4fv(scene.textured_mvp_loc, 1, GL_FALSE, mvp.m);
        gl.Uniform1i(scene.textured_sampler_loc, 0);
        gl.BindVertexArray(scene.textured_vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, scene.textured_vbo);
        for (const TexturedQuad &quad : quads) {
            gl.ActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, quad.texture);
            gl.BufferData(GL_ARRAY_BUFFER, (ptrdiff_t)sizeof(quad.vertices), quad.vertices.data(),
                          GL_DYNAMIC_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)quad.vertices.size());
        }
        gl.BindVertexArray(0);
    }

    if (!lines.empty()) {
        glLineWidth(2.0f);
        gl.UseProgram(scene.line_shader_program);
        gl.UniformMatrix4fv(scene.line_mvp_loc, 1, GL_FALSE, mvp.m);
        gl.BindVertexArray(scene.line_vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, scene.line_vbo);
        gl.BufferData(GL_ARRAY_BUFFER, (ptrdiff_t)(lines.size() * sizeof(LineVertex)),
                      lines.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)lines.size());
        gl.BindVertexArray(0);
    }

    gl.UseProgram(0);
    glDisable(GL_DEPTH_TEST);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(uintptr_t)scene.fbo_texture, ImVec2((float)w, (float)h),
                 ImVec2(0, 1), ImVec2(1, 0));

    ImGui::SetCursorScreenPos(cursor);
    ImGui::InvisibleButton("##3d_interact", ImVec2((float)w, (float)h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool is_hovered = ImGui::IsItemHovered();
    bool is_active = ImGui::IsItemActive();
    if (is_hovered || is_active) {
        ImGuiIO &io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImVec2 delta = io.MouseDelta;
            scene.camera.rotate(-delta.x * 0.005f, -delta.y * 0.005f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            ImVec2 delta = io.MouseDelta;
            scene.camera.pan(delta.x, delta.y);
        }
        if (is_hovered && io.MouseWheel != 0.0f)
            scene.camera.zoom(io.MouseWheel);
    }

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    std::vector<ScreenHoverLayer> screen_hover_layers;
    screen_hover_layers.reserve(hover_layers.size());
    for (const HoverLayerInfo &hover_info : hover_layers) {
        ScreenHoverLayer screen_layer;
        screen_layer.info = &hover_info;
        screen_layer.color = ImGui::ColorConvertFloat4ToU32(hover_info.color);
        if (project_quad_to_screen(mvp, hover_info.world_corners, (float)w, (float)h, cursor,
                                   screen_layer.corners, screen_layer.center)) {
            screen_hover_layers.push_back(screen_layer);
        }
    }

    const ScreenHoverLayer *hovered_layer = nullptr;
    if (is_hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        ImVec2 mouse_pos = ImGui::GetIO().MousePos;
        for (auto it = screen_hover_layers.rbegin(); it != screen_hover_layers.rend(); ++it) {
            if (point_in_quad_2d(mouse_pos, it->corners)) {
                hovered_layer = &*it;
                break;
            }
        }
    }

    for (const ScreenHoverLayer &screen_layer : screen_hover_layers) {
        const HoverLayerInfo &hover_info = *screen_layer.info;
        ImVec2 text_pos(screen_layer.center.x + 6.0f, screen_layer.center.y - 12.0f);
        ImVec2 text_size = ImGui::CalcTextSize(hover_info.overlay_text.c_str());
        ImU32 bg_color = IM_COL32(10, 12, 16, 185);
        draw_list->AddRectFilled(ImVec2(text_pos.x - 4.0f, text_pos.y - 2.0f),
                                 ImVec2(text_pos.x + text_size.x + 4.0f,
                                        text_pos.y + text_size.y + 2.0f),
                                 bg_color, 3.0f);
        draw_list->AddText(text_pos, screen_layer.color, hover_info.overlay_text.c_str());
        if (hovered_layer == &screen_layer) {
            draw_list->AddPolyline(screen_layer.corners.data(), (int)screen_layer.corners.size(),
                                   IM_COL32(255, 255, 255, 230), ImDrawFlags_Closed, 2.5f);
        }
    }

    if (hovered_layer != nullptr)
        render_hover_layer_tooltip(*hovered_layer->info);

    struct ScreenLabel {
        float sx, sy;
        std::string text;
        ImU32 color;
    };
    std::vector<ScreenLabel> screen_labels;
    screen_labels.reserve(labels.size());

    for (const LabelInfo &label : labels) {
        float sx = 0.0f;
        float sy = 0.0f;
        if (project_to_screen(mvp, label.pos, (float)w, (float)h, sx, sy)) {
            screen_labels.push_back({cursor.x + sx, cursor.y + sy, label.text,
                                     ImGui::ColorConvertFloat4ToU32(label.color)});
        }
    }

    std::sort(screen_labels.begin(), screen_labels.end(),
              [](const ScreenLabel &a, const ScreenLabel &b) {
                  if (std::abs(a.sx - b.sx) < 60.0f)
                      return a.sy < b.sy;
                  return a.sx < b.sx;
              });

    float label_height = ImGui::GetFontSize() + 2.0f;
    for (size_t i = 1; i < screen_labels.size(); i++) {
        for (size_t j = 0; j < i; j++) {
            float dx = std::abs(screen_labels[i].sx - screen_labels[j].sx);
            float dy = screen_labels[i].sy - screen_labels[j].sy;
            if (dx < 100.0f && dy >= 0.0f && dy < label_height)
                screen_labels[i].sy = screen_labels[j].sy + label_height;
        }
    }

    for (const ScreenLabel &sl : screen_labels) {
        draw_list->AddCircleFilled(ImVec2(sl.sx, sl.sy), 3.0f, sl.color);
        draw_list->AddText(ImVec2(sl.sx + 5.0f, sl.sy - 8.0f), sl.color, sl.text.c_str());
    }

    ImGui::End();
}

} // namespace debug_layer
