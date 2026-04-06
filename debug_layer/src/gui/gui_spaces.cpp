// SPDX-License-Identifier: MIT
// gui/gui_spaces.cpp — 3D space visualization panel with arcball camera.

#include "gui_spaces.h"
#include "gui_common.h"
#include "../instance_data.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <string>
#include <vector>

// We need raw GL calls for the 3D scene FBO rendering.
// SDL3's OpenGL context gives us GL functions; we use a small subset.
#include <SDL3/SDL_opengl.h>

namespace debug_layer {

// ── GL function pointers (loaded once via SDL_GL_GetProcAddress) ─────────────

static struct {
    bool loaded = false;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
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
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
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
    LOAD(GenVertexArrays);
    LOAD(BindVertexArray);
    LOAD(DeleteVertexArrays);
    LOAD(EnableVertexAttribArray);
    LOAD(VertexAttribPointer);
#undef LOAD
    gl.loaded = true;
}

// ── Simple line vertex: position + color ─────────────────────────────────────

struct LineVertex {
    float x, y, z;
    float r, g, b, a;
};

// ── Persistent scene resources ───────────────────────────────────────────────

static struct SceneState {
    bool initialized = false;
    GLuint fbo = 0;
    GLuint fbo_texture = 0;
    int fbo_width = 0;
    int fbo_height = 0;
    GLuint shader_program = 0;
    GLint mvp_loc = -1;
    GLuint vao = 0;
    GLuint vbo = 0;
    ArcballCamera camera;
} scene;

static void ensure_fbo(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    if (scene.fbo != 0 && scene.fbo_width == width && scene.fbo_height == height)
        return;

    // Delete old
    if (scene.fbo != 0) {
        gl.DeleteFramebuffers(1, &scene.fbo);
        glDeleteTextures(1, &scene.fbo_texture);
    }

    scene.fbo_width = width;
    scene.fbo_height = height;

    // Create texture
    glGenTextures(1, &scene.fbo_texture);
    glBindTexture(GL_TEXTURE_2D, scene.fbo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create FBO
    gl.GenFramebuffers(1, &scene.fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            scene.fbo_texture, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void ensure_shader()
{
    if (scene.shader_program != 0)
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

    GLuint vs = gl.CreateShader(GL_VERTEX_SHADER);
    gl.ShaderSource(vs, 1, &vs_src, nullptr);
    gl.CompileShader(vs);

    GLuint fs = gl.CreateShader(GL_FRAGMENT_SHADER);
    gl.ShaderSource(fs, 1, &fs_src, nullptr);
    gl.CompileShader(fs);

    scene.shader_program = gl.CreateProgram();
    gl.AttachShader(scene.shader_program, vs);
    gl.AttachShader(scene.shader_program, fs);
    gl.LinkProgram(scene.shader_program);

    gl.DeleteShader(vs);
    gl.DeleteShader(fs);

    scene.mvp_loc = gl.GetUniformLocation(scene.shader_program, "uMVP");
}

static void ensure_vao_vbo()
{
    if (scene.vao != 0)
        return;

    gl.GenVertexArrays(1, &scene.vao);
    gl.GenBuffers(1, &scene.vbo);

    gl.BindVertexArray(scene.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, scene.vbo);

    // Position (location 0)
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), (void *)0);

    // Color (location 1)
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                           (void *)(3 * sizeof(float)));

    gl.BindVertexArray(0);
}

// ── Geometry builders ────────────────────────────────────────────────────────

static void add_line(std::vector<LineVertex> &verts, Vec3 a, Vec3 b, float r, float g, float bl,
                     float alpha = 1.0f)
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

static void add_axes(std::vector<LineVertex> &verts, Vec3 origin, float length, float alpha = 1.0f)
{
    add_line(verts, origin, origin + Vec3{length, 0, 0}, 1.0f, 0.2f, 0.2f, alpha); // X = red
    add_line(verts, origin, origin + Vec3{0, length, 0}, 0.2f, 1.0f, 0.2f, alpha); // Y = green
    add_line(verts, origin, origin + Vec3{0, 0, length}, 0.2f, 0.2f, 1.0f, alpha); // Z = blue
}

static void add_coordinate_frame(std::vector<LineVertex> &verts, const XrPosef &pose, float size,
                                 float alpha = 1.0f)
{
    Mat4 m = Mat4::from_quat_pos(pose.orientation.x, pose.orientation.y, pose.orientation.z,
                                 pose.orientation.w, pose.position.x, pose.position.y,
                                 pose.position.z);

    Vec3 origin{pose.position.x, pose.position.y, pose.position.z};

    // Transform local axes through the rotation
    Vec4 rx = m * Vec4{size, 0, 0, 0};
    Vec4 ry = m * Vec4{0, size, 0, 0};
    Vec4 rz = m * Vec4{0, 0, size, 0};

    add_line(verts, origin, origin + Vec3{rx.x, rx.y, rx.z}, 1.0f, 0.2f, 0.2f, alpha);
    add_line(verts, origin, origin + Vec3{ry.x, ry.y, ry.z}, 0.2f, 1.0f, 0.2f, alpha);
    add_line(verts, origin, origin + Vec3{rz.x, rz.y, rz.z}, 0.2f, 0.2f, 1.0f, alpha);
}

// ── FOV frustum wireframe ────────────────────────────────────────────────────

static void compute_frustum_corners(const XrPosef &pose, const XrFovf &fov,
                                    float dist, Vec3 corners[4])
{
    // Compute the 4 corners of a plane at `dist` from the pose along -Z (OpenXR convention)
    float tan_left = std::tan(fov.angleLeft);
    float tan_right = std::tan(fov.angleRight);
    float tan_up = std::tan(fov.angleUp);
    float tan_down = std::tan(fov.angleDown);

    float l = tan_left * dist;
    float r = tan_right * dist;
    float u = tan_up * dist;
    float d = tan_down * dist;

    Mat4 m = Mat4::from_quat_pos(pose.orientation.x, pose.orientation.y, pose.orientation.z,
                                 pose.orientation.w, pose.position.x, pose.position.y,
                                 pose.position.z);

    // Local corners (OpenXR: -Z is forward, +X is right, +Y is up)
    Vec4 local[4] = {
        {l, u, -dist, 1.0f}, // top-left
        {r, u, -dist, 1.0f}, // top-right
        {r, d, -dist, 1.0f}, // bottom-right
        {l, d, -dist, 1.0f}, // bottom-left
    };

    for (int i = 0; i < 4; i++) {
        Vec4 world = m * local[i];
        corners[i] = {world.x, world.y, world.z};
    }
}

static void add_fov_frustum(std::vector<LineVertex> &verts, const XrPosef &pose,
                            const XrFovf &fov, float near_dist, float far_dist,
                            float r, float g, float b, float alpha)
{
    Vec3 origin{pose.position.x, pose.position.y, pose.position.z};

    Vec3 near_corners[4], far_corners[4];
    compute_frustum_corners(pose, fov, near_dist, near_corners);
    compute_frustum_corners(pose, fov, far_dist, far_corners);

    // Lines from origin to far corners
    for (int i = 0; i < 4; i++)
        add_line(verts, origin, far_corners[i], r, g, b, alpha * 0.4f);

    // Near rectangle
    for (int i = 0; i < 4; i++)
        add_line(verts, near_corners[i], near_corners[(i + 1) % 4], r, g, b, alpha);

    // Far rectangle
    for (int i = 0; i < 4; i++)
        add_line(verts, far_corners[i], far_corners[(i + 1) % 4], r, g, b, alpha * 0.5f);
}

static void add_view_rectangle(std::vector<LineVertex> &verts, const XrPosef &pose,
                                const XrFovf &fov, float dist,
                                float r, float g, float b, float alpha)
{
    Vec3 corners[4];
    compute_frustum_corners(pose, fov, dist, corners);

    // Draw filled-looking rectangle using edge lines and cross
    for (int i = 0; i < 4; i++)
        add_line(verts, corners[i], corners[(i + 1) % 4], r, g, b, alpha);

    // Diagonal cross for visibility
    add_line(verts, corners[0], corners[2], r, g, b, alpha * 0.3f);
    add_line(verts, corners[1], corners[3], r, g, b, alpha * 0.3f);
}

// ── Space color by type ──────────────────────────────────────────────────────

static ImVec4 color_for_space(const TrackedSpace &sp)
{
    if (sp.kind == SpaceKind::REFERENCE) {
        switch (sp.reference_type) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: return {1.0f, 0.6f, 0.2f, 1.0f};   // orange
        case XR_REFERENCE_SPACE_TYPE_LOCAL: return {0.3f, 0.5f, 1.0f, 1.0f};   // blue
        case XR_REFERENCE_SPACE_TYPE_STAGE: return {0.3f, 1.0f, 0.3f, 1.0f};   // green
        default: return {0.8f, 0.8f, 0.8f, 1.0f};
        }
    }
    return {1.0f, 1.0f, 0.3f, 1.0f}; // yellow for action spaces
}

// ── Main render function ─────────────────────────────────────────────────────

void gui_render_spaces_panel(InstanceData *data)
{
    load_gl_functions();

    ImGui::Begin("3D Spaces");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = (int)avail.x;
    int h = (int)avail.y;
    if (w < 10)
        w = 10;
    if (h < 10)
        h = 10;

    // Ensure GL resources
    ensure_fbo(w, h);
    ensure_shader();
    ensure_vao_vbo();

    if (scene.fbo == 0 || scene.shader_program == 0)
    {
        ImGui::TextDisabled("GL resources not available.");
        ImGui::End();
        return;
    }

    // ── Build line geometry ──────────────────────────────────────────────
    std::vector<LineVertex> lines;

    // Grid
    add_grid(lines, 5.0f, 1.0f);

    // Origin axes
    add_axes(lines, {0, 0, 0}, 0.5f);

    // Tracked spaces
    struct LabelInfo {
        Vec3 pos;
        std::string text;
        ImVec4 color;
    };
    std::vector<LabelInfo> labels;

    {
        std::shared_lock lock(data->state_mutex);
        for (auto &[handle, sp] : data->spaces) {
            if (!sp.has_location)
                continue;

            bool valid = (sp.latest_location.locationFlags &
                          XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            if (!valid)
                continue;

            XrPosef pose = sp.latest_location.pose;
            float alpha = 1.0f;

            // For action spaces, check if the action is active
            if (sp.kind == SpaceKind::ACTION) {
                ActionStateKey key{sp.action, sp.subaction_path};
                auto sit = data->action_states.find(key);
                if (sit != data->action_states.end() && !sit->second.is_active)
                    alpha = 0.3f;
            }

            float frame_size = (sp.kind == SpaceKind::REFERENCE) ? 0.3f : 0.2f;
            add_coordinate_frame(lines, pose, frame_size, alpha);

            Vec3 pos{pose.position.x, pose.position.y, pose.position.z};
            labels.push_back({pos, sp.label(), color_for_space(sp)});
        }

        // ── View poses (from xrLocateViews) ──────────────────────────────
        for (auto &vp : data->view_poses) {
            if (!vp.valid)
                continue;

            add_coordinate_frame(lines, vp.pose, 0.15f, 1.0f);

            // Draw FOV frustum wireframe
            add_fov_frustum(lines, vp.pose, vp.fov, 0.01f, 0.2f, // near, far
                            0.9f, 0.7f, 0.2f, 0.5f);             // orange, semi-transparent

            // Draw view rectangle at near plane
            add_view_rectangle(lines, vp.pose, vp.fov, 0.01f,
                               0.9f, 0.7f, 0.2f, 0.8f);

            Vec3 pos{vp.pose.position.x, vp.pose.position.y, vp.pose.position.z};
            labels.push_back({pos, vp.label, {1.0f, 0.7f, 0.2f, 1.0f}});
        }
    }

    // ── Render to FBO ────────────────────────────────────────────────────
    gl.BindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
    glViewport(0, 0, w, h);
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(2.0f);

    // Build MVP
    float aspect = (float)w / (float)h;
    Mat4 proj = Mat4::perspective(0.9f, aspect, 0.1f, 100.0f); // ~51 deg FOV
    Mat4 view = scene.camera.get_view();
    Mat4 mvp = proj * view;

    gl.UseProgram(scene.shader_program);
    gl.UniformMatrix4fv(scene.mvp_loc, 1, GL_FALSE, mvp.m);

    // Upload and draw lines
    if (!lines.empty()) {
        gl.BindVertexArray(scene.vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, scene.vbo);
        gl.BufferData(GL_ARRAY_BUFFER, (ptrdiff_t)(lines.size() * sizeof(LineVertex)), lines.data(),
                      GL_DYNAMIC_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)lines.size());
        gl.BindVertexArray(0);
    }

    gl.UseProgram(0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    // ── Display FBO texture in ImGui ─────────────────────────────────────
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(uintptr_t)scene.fbo_texture, ImVec2((float)w, (float)h),
                 ImVec2(0, 1), ImVec2(1, 0)); // flip Y

    // ── Handle mouse input on the image area ─────────────────────────────
    // Use InvisibleButton overlaid on the image so that mouse drags are captured
    // by this widget rather than dragging/moving the window.
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
        if (is_hovered && io.MouseWheel != 0.0f) {
            scene.camera.zoom(io.MouseWheel);
        }
    }

    // ── Draw labels as overlays (with anti-overlap) ──────────────────────
    ImDrawList *draw_list = ImGui::GetWindowDrawList();

    // Project all labels and sort by screen Y for overlap avoidance
    struct ScreenLabel {
        float sx, sy;
        std::string text;
        ImU32 color;
    };
    std::vector<ScreenLabel> screen_labels;
    screen_labels.reserve(labels.size());

    for (auto &label : labels) {
        float sx, sy;
        if (project_to_screen(mvp, label.pos, (float)w, (float)h, sx, sy)) {
            screen_labels.push_back({
                cursor.x + sx, cursor.y + sy,
                label.text,
                ImGui::ColorConvertFloat4ToU32(label.color)
            });
        }
    }

    // Sort by screen X then Y for stable ordering
    std::sort(screen_labels.begin(), screen_labels.end(),
              [](const ScreenLabel &a, const ScreenLabel &b) {
                  if (std::abs(a.sx - b.sx) < 60.0f)
                      return a.sy < b.sy;
                  return a.sx < b.sx;
              });

    // Nudge overlapping labels apart
    float label_height = ImGui::GetFontSize() + 2.0f;
    for (size_t i = 1; i < screen_labels.size(); i++) {
        for (size_t j = 0; j < i; j++) {
            float dx = std::abs(screen_labels[i].sx - screen_labels[j].sx);
            float dy = screen_labels[i].sy - screen_labels[j].sy;
            if (dx < 100.0f && dy >= 0 && dy < label_height) {
                screen_labels[i].sy = screen_labels[j].sy + label_height;
            }
        }
    }

    for (auto &sl : screen_labels) {
        // Draw dot at original projected position
        draw_list->AddCircleFilled(ImVec2(sl.sx, sl.sy), 3.0f, sl.color);
        draw_list->AddText(ImVec2(sl.sx + 5, sl.sy - 8), sl.color, sl.text.c_str());
    }

    ImGui::End();
}

} // namespace debug_layer
