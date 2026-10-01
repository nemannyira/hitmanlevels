#include "guides.h"

#include <cmath>

#include "../display.h"
#include "../fov.h"
#include "../game.h"

namespace h2::editor {

namespace {

constexpr float kNear = 5.0f;  // cm in front of the camera

struct Camera {
    float pos[3], f[3], u[3], r[3];
    float tanH, tanV, w, h;
};

bool CurrentCamera(Camera& cam)
{
    game::ZCAMERA* c = game::MainCamera();
    if (!c || RenderWidth() <= 0)
        return false;
    float m[9];
    game::GetWorldTransform(c, m, cam.pos);
    for (int i = 0; i < 3; ++i) {
        cam.f[i] = m[i];
        cam.u[i] = m[3 + i];
        cam.r[i] = m[6 + i];
    }
    cam.tanH = std::tan(c->fov * 0.5f) * HorizontalTanScale();
    cam.w = float(RenderWidth());
    cam.h = float(RenderHeight());
    cam.tanV = cam.tanH * cam.h / cam.w;
    return true;
}

void ToView(const Camera& cam, const float p[3], float v[3])
{
    float d[3] = {p[0] - cam.pos[0], p[1] - cam.pos[1], p[2] - cam.pos[2]};
    v[0] = d[0] * cam.r[0] + d[1] * cam.r[1] + d[2] * cam.r[2];
    v[1] = d[0] * cam.u[0] + d[1] * cam.u[1] + d[2] * cam.u[2];
    v[2] = d[0] * cam.f[0] + d[1] * cam.f[1] + d[2] * cam.f[2];
}

ImVec2 Project(const Camera& cam, const float v[3])
{
    return ImVec2((v[0] / v[2] / cam.tanH * 0.5f + 0.5f) * cam.w, (0.5f - v[1] / v[2] / cam.tanV * 0.5f) * cam.h);
}

// Clips the view-space segment to z >= kNear; false when entirely behind.
bool Clip(float a[3], float b[3])
{
    if (a[2] < kNear && b[2] < kNear)
        return false;
    if (a[2] < kNear || b[2] < kNear) {
        float* behind = a[2] < kNear ? a : b;
        const float* front = a[2] < kNear ? b : a;
        float t = (kNear - behind[2]) / (front[2] - behind[2]);
        for (int i = 0; i < 3; ++i)
            behind[i] += (front[i] - behind[i]) * t;
    }
    return true;
}

void Line(const Camera& cam, const float a[3], const float b[3], ImU32 color, float thickness)
{
    float va[3], vb[3];
    ToView(cam, a, va);
    ToView(cam, b, vb);
    if (!Clip(va, vb))
        return;
    ImGui::GetBackgroundDrawList()->AddLine(Project(cam, va), Project(cam, vb), color, thickness);
}

ImU32 WithAlpha(ImU32 color, float alpha)
{
    unsigned a = unsigned(((color >> IM_COL32_A_SHIFT) & 0xFF) * (alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha)));
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Normalize(float v[3])
{
    float l = std::sqrt(Dot(v, v));
    if (l > 1e-8f)
        for (int i = 0; i < 3; ++i)
            v[i] /= l;
}

// The 4 corners of a box face (axis k, positive side or not), in order around it.
void FaceCorners(int k, bool positive, int out[4])
{
    int a = (k + 1) % 3, b = (k + 2) % 3;
    int base = positive ? (1 << k) : 0;
    out[0] = base;
    out[1] = base | (1 << a);
    out[2] = base | (1 << a) | (1 << b);
    out[3] = base | (1 << b);
}

// Face whose outward normal is most along `dir`.
void FaceAlong(const Box& box, const float dir[3], int corners[4])
{
    float best = -2;
    for (int k = 0; k < 3; ++k) {
        // box axis k: from corner 0 to corner (1 << k)
        float axis[3];
        for (int i = 0; i < 3; ++i)
            axis[i] = box.c[1 << k][i] - box.c[0][i];
        Normalize(axis);
        for (int s = 0; s < 2; ++s) {
            float d = Dot(axis, dir) * (s ? 1.0f : -1.0f);
            if (d > best) {
                best = d;
                FaceCorners(k, s != 0, corners);
            }
        }
    }
}

} // namespace

bool ObjectBox(engine::ZGEOM* geom, const Transform& t, Box& box)
{
    if (!geom || !IsLive(geom))
        return false;
    const engine::ZBaseGeom* n = geom->node;
    float c[3] = {n->boundCenter[0], n->boundCenter[1], n->boundCenter[2]};
    float h[3] = {n->boundHalf[0], n->boundHalf[1], n->boundHalf[2]};
    if (!(h[0] >= 0 && h[1] >= 0 && h[2] >= 0) || h[0] + h[1] + h[2] <= 0.0f || h[0] + h[1] + h[2] > 1e6f) {
        float r = n->boundRadius > 1.0f && n->boundRadius < 1e5f ? n->boundRadius : 25.0f;
        h[0] = h[1] = h[2] = r * 0.577f;
    }
    const float* f = t.mat;
    const float* u = t.mat + 3;
    const float* r = t.mat + 6;
    for (int i = 0; i < 8; ++i) {
        // node space: x = right, y = up, z = forward
        float lx = c[0] + ((i & 1) ? h[0] : -h[0]);
        float ly = c[1] + ((i & 2) ? h[1] : -h[1]);
        float lz = c[2] + ((i & 4) ? h[2] : -h[2]);
        for (int k = 0; k < 3; ++k)
            box.c[i][k] = t.pos[k] + r[k] * lx + u[k] * ly + f[k] * lz;
    }
    return true;
}

void BoxBottom(const Box& box, const float normal[3], float out[3])
{
    float down[3] = {-normal[0], -normal[1], -normal[2]};
    int face[4];
    FaceAlong(box, down, face);
    for (int k = 0; k < 3; ++k)
        out[k] = (box.c[face[0]][k] + box.c[face[1]][k] + box.c[face[2]][k] + box.c[face[3]][k]) * 0.25f;
}

float GridStep(float size)
{
    const float steps[] = {10, 25, 50, 100, 200, 500, 1000};
    for (float s : steps)
        if (s >= size / 4.0f)
            return s;
    return steps[6];
}

void DrawLine3D(const float a[3], const float b[3], ImU32 color, float thickness)
{
    Camera cam;
    if (CurrentCamera(cam))
        Line(cam, a, b, color, thickness);
}

void DrawBox(const Box& box, ImU32 color, float thickness)
{
    Camera cam;
    if (!CurrentCamera(cam))
        return;
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& e : edges)
        Line(cam, box.c[e[0]], box.c[e[1]], color, thickness);
}

void DrawGrid(const float center[3], const float u[3], const float v[3], float step, float radius, ImU32 color,
              bool axes)
{
    Camera cam;
    if (!CurrentCamera(cam) || step <= 0 || radius <= 0)
        return;
    int n = int(std::ceil(radius / step));
    if (n > 60)
        n = 60, step = radius / 60.0f;
    // absolute grid of the plane: lines where dot(p, u) and dot(p, v) are multiples of step
    float cu = Dot(center, u), cv = Dot(center, v);
    float ou = std::floor(cu / step) * step - cu, ov = std::floor(cv / step) * step - cv;
    const int kPieces = 10;  // per line, for the fade
    auto point = [&](float a, float b, float out[3]) {
        for (int k = 0; k < 3; ++k)
            out[k] = center[k] + u[k] * a + v[k] * b;
    };
    auto fade = [&](float a, float b) {
        float d = std::sqrt(a * a + b * b) / radius;
        return d >= 1.0f ? 0.0f : (1.0f - d * d);
    };
    for (int dir = 0; dir < 2; ++dir) {
        float offset = dir == 0 ? ou : ov;
        float base = dir == 0 ? cu : cv;
        for (int i = -n; i <= n + 1; ++i) {
            float a = offset + i * step;  // across
            if (std::fabs(a) > radius)
                continue;
            long index = std::lround((base + a) / step);
            bool major = index % 5 == 0;
            float half = std::sqrt(radius * radius - a * a);
            for (int p = 0; p < kPieces; ++p) {
                float b0 = -half + 2 * half * p / kPieces, b1 = -half + 2 * half * (p + 1) / kPieces;
                float alpha = fade(a, (b0 + b1) * 0.5f) * (major ? 1.0f : 0.45f);
                if (alpha <= 0.02f)
                    continue;
                float p0[3], p1[3];
                if (dir == 0)
                    point(a, b0, p0), point(a, b1, p1);
                else
                    point(b0, a, p0), point(b1, a, p1);
                Line(cam, p0, p1, WithAlpha(color, alpha), major ? 1.5f : 1.0f);
            }
        }
    }
    if (axes) {
        for (int dir = 0; dir < 2; ++dir) {
            for (int p = 0; p < kPieces * 2; ++p) {
                float b0 = -radius + 2 * radius * p / (kPieces * 2), b1 = -radius + 2 * radius * (p + 1) / (kPieces * 2);
                float alpha = fade(0, (b0 + b1) * 0.5f);
                float p0[3], p1[3];
                if (dir == 0)
                    point(b0, 0, p0), point(b1, 0, p1);  // along u
                else
                    point(0, b0, p0), point(0, b1, p1);  // along v
                Line(cam, p0, p1, WithAlpha(dir == 0 ? kAxisX : kAxisZ, alpha * 0.9f), 2.0f);
            }
        }
    }
}

void DrawFootprint(const Box& box, const float point[3], const float normal[3], ImU32 fill, ImU32 outline)
{
    Camera cam;
    if (!CurrentCamera(cam))
        return;
    float down[3] = {-normal[0], -normal[1], -normal[2]};
    int face[4];
    FaceAlong(box, down, face);
    float proj[4][3];
    ImVec2 screen[4];
    bool visible = true;
    for (int i = 0; i < 4; ++i) {
        const float* c = box.c[face[i]];
        float d = (c[0] - point[0]) * normal[0] + (c[1] - point[1]) * normal[1] + (c[2] - point[2]) * normal[2];
        for (int k = 0; k < 3; ++k)
            proj[i][k] = c[k] - normal[k] * d;
        if (std::fabs(d) > 1.0f)  // drop line from the corner to the plane
            Line(cam, c, proj[i], WithAlpha(outline, 0.6f), 1.0f);
        float v[3];
        ToView(cam, proj[i], v);
        visible &= v[2] >= kNear;
        if (visible)
            screen[i] = Project(cam, v);
    }
    if (visible) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddConvexPolyFilled(screen, 4, fill);
        dl->AddPolyline(screen, 4, outline, ImDrawFlags_Closed, 2.0f);
    } else {
        for (int i = 0; i < 4; ++i)
            Line(cam, proj[i], proj[(i + 1) % 4], outline, 2.0f);
    }
}

void DrawSnapTarget(const float p[3], ImU32 color)
{
    Camera cam;
    if (!CurrentCamera(cam))
        return;
    float v[3];
    ToView(cam, p, v);
    if (v[2] < kNear)
        return;
    ImVec2 s = Project(cam, v);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCircle(s, 9.0f, IM_COL32(0, 0, 0, 160), 24, 4.0f);
    dl->AddCircle(s, 9.0f, color, 24, 2.0f);
    dl->AddCircleFilled(s, 2.5f, color);
}

void PlaneAxes(const float normal[3], float u[3], float v[3])
{
    if (std::fabs(normal[1]) > 0.7f) {  // floor or ceiling: world X and Z
        float x[3] = {1, 0, 0}, z[3] = {0, 0, 1};
        // project onto the plane so slopes keep the grid on the surface
        for (int k = 0; k < 3; ++k) {
            u[k] = x[k] - normal[k] * normal[0];
            v[k] = z[k] - normal[k] * normal[2];
        }
    } else {  // wall: horizontal direction along it, and "up" in it
        float up[3] = {0, 1, 0};
        u[0] = up[1] * normal[2] - up[2] * normal[1];
        u[1] = up[2] * normal[0] - up[0] * normal[2];
        u[2] = up[0] * normal[1] - up[1] * normal[0];
        Normalize(u);
        v[0] = normal[1] * u[2] - normal[2] * u[1];
        v[1] = normal[2] * u[0] - normal[0] * u[2];
        v[2] = normal[0] * u[1] - normal[1] * u[0];
    }
    Normalize(u);
    Normalize(v);
}

} // namespace h2::editor
