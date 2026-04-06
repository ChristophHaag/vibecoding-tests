// SPDX-License-Identifier: MIT
// gui_common.h — Lightweight math helpers for 3D rendering (mat4, vec3, arcball).
#pragma once

#include <cmath>
#include <cstring>

namespace debug_layer {

// ── Vec3 ─────────────────────────────────────────────────────────────────────

struct Vec3 {
    float x, y, z;

    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3 &b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec3 operator-(const Vec3 &b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }

    float dot(const Vec3 &b) const { return x * b.x + y * b.y + z * b.z; }
    float length() const { return std::sqrt(dot(*this)); }

    Vec3 normalized() const
    {
        float l = length();
        return l > 1e-8f ? Vec3{x / l, y / l, z / l} : Vec3{0, 0, 0};
    }

    Vec3 cross(const Vec3 &b) const
    {
        return {y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x};
    }
};

// ── Vec4 ─────────────────────────────────────────────────────────────────────

struct Vec4 {
    float x, y, z, w;
    Vec4() : x(0), y(0), z(0), w(0) {}
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

// ── Mat4 (column-major, OpenGL convention) ───────────────────────────────────

struct Mat4 {
    float m[16]; // column-major: m[col*4 + row]

    Mat4() { std::memset(m, 0, sizeof(m)); }

    static Mat4 identity()
    {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    float &at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    Mat4 operator*(const Mat4 &b) const
    {
        Mat4 r;
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++) {
                float sum = 0;
                for (int k = 0; k < 4; k++)
                    sum += at(row, k) * b.at(k, col);
                r.at(row, col) = sum;
            }
        return r;
    }

    Vec4 operator*(const Vec4 &v) const
    {
        return {
            m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
            m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
            m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w,
        };
    }

    static Mat4 perspective(float fovy_rad, float aspect, float near, float far)
    {
        float f = 1.0f / std::tan(fovy_rad * 0.5f);
        Mat4 r;
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (far + near) / (near - far);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * far * near) / (near - far);
        return r;
    }

    static Mat4 look_at(const Vec3 &eye, const Vec3 &center, const Vec3 &up)
    {
        Vec3 f = (center - eye).normalized();
        Vec3 s = f.cross(up).normalized();
        Vec3 u = s.cross(f);

        Mat4 r = identity();
        r.at(0, 0) = s.x;
        r.at(0, 1) = s.y;
        r.at(0, 2) = s.z;
        r.at(1, 0) = u.x;
        r.at(1, 1) = u.y;
        r.at(1, 2) = u.z;
        r.at(2, 0) = -f.x;
        r.at(2, 1) = -f.y;
        r.at(2, 2) = -f.z;
        r.at(0, 3) = -s.dot(eye);
        r.at(1, 3) = -u.dot(eye);
        r.at(2, 3) = f.dot(eye);
        return r;
    }

    static Mat4 translate(const Vec3 &t)
    {
        Mat4 r = identity();
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 from_quat_pos(float qx, float qy, float qz, float qw, float px, float py, float pz)
    {
        Mat4 r = identity();
        float xx = qx * qx, yy = qy * qy, zz = qz * qz;
        float xy = qx * qy, xz = qx * qz, yz = qy * qz;
        float wx = qw * qx, wy = qw * qy, wz = qw * qz;

        r.at(0, 0) = 1 - 2 * (yy + zz);
        r.at(1, 0) = 2 * (xy + wz);
        r.at(2, 0) = 2 * (xz - wy);
        r.at(0, 1) = 2 * (xy - wz);
        r.at(1, 1) = 1 - 2 * (xx + zz);
        r.at(2, 1) = 2 * (yz + wx);
        r.at(0, 2) = 2 * (xz + wy);
        r.at(1, 2) = 2 * (yz - wx);
        r.at(2, 2) = 1 - 2 * (xx + yy);
        r.at(0, 3) = px;
        r.at(1, 3) = py;
        r.at(2, 3) = pz;
        return r;
    }
};

// ── Arcball camera state ─────────────────────────────────────────────────────

struct ArcballCamera {
    Vec3 target = {0, 1.0f, 0};
    float distance = 5.0f;
    float yaw = 0.3f;   // radians
    float pitch = 0.4f;  // radians

    float min_distance = 0.5f;
    float max_distance = 50.0f;
    float min_pitch = -1.5f;
    float max_pitch = 1.5f;

    void rotate(float dyaw, float dpitch)
    {
        yaw += dyaw;
        pitch += dpitch;
        if (pitch < min_pitch)
            pitch = min_pitch;
        if (pitch > max_pitch)
            pitch = max_pitch;
    }

    void pan(float dx, float dy)
    {
        // Pan in the camera's local right/up plane
        Vec3 eye_pos = get_eye();
        Vec3 fwd = (target - eye_pos).normalized();
        Vec3 right = fwd.cross(Vec3{0, 1, 0}).normalized();
        Vec3 up = right.cross(fwd).normalized();
        target = target + right * (-dx * distance * 0.002f) + up * (dy * distance * 0.002f);
    }

    void zoom(float delta)
    {
        distance *= (1.0f - delta * 0.1f);
        if (distance < min_distance)
            distance = min_distance;
        if (distance > max_distance)
            distance = max_distance;
    }

    Vec3 get_eye() const
    {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        float cy = std::cos(yaw), sy = std::sin(yaw);
        return target + Vec3{cp * sy, sp, cp * cy} * distance;
    }

    Mat4 get_view() const
    {
        return Mat4::look_at(get_eye(), target, Vec3{0, 1, 0});
    }
};

// ── Project 3D → 2D screen coordinates ──────────────────────────────────────

inline bool project_to_screen(const Mat4 &mvp, const Vec3 &pos, float viewport_w, float viewport_h,
                              float &screen_x, float &screen_y)
{
    Vec4 clip = mvp * Vec4{pos.x, pos.y, pos.z, 1.0f};
    if (clip.w < 0.001f)
        return false;
    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;
    screen_x = (ndc_x * 0.5f + 0.5f) * viewport_w;
    screen_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * viewport_h; // flip Y for screen coords
    return ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f;
}

} // namespace debug_layer
