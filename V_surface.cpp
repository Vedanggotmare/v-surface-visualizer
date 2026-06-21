// =============================================================================
//  V(t,x) Surface Visualizer — v2
//
//  V(t,x) = g * sqrt(sqrt(2)*s/pi) * exp(-r0/2*(T^2-t^2)) * (T-t)^(1/4)
//           * Gamma(5/8) * 1F1(-1/4; 1/2; -z)
//  z = [x + (mu0/lam)*(sin(lam*T)-sin(lam*t))] / (2*s^2*(T-t))
//
//  Build:  cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
//          cmake --build build -j4
// =============================================================================

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif
#define _USE_MATH_DEFINES
#include <cmath>
#include <vector>
#include <algorithm>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <cfloat>
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

// =============================================================================
//  Kummer Confluent Hypergeometric  1F1(a; b; z)
// =============================================================================
static double kummer(double a, double b, double z) {
    if (!std::isfinite(z)) return 0.0;

    // Large positive z — use asymptotic:  Gamma(b)/Gamma(a) * e^z * z^{a-b}
    if (z > 15.0) {
        double lga = std::lgamma(std::abs(a));
        double lgb = std::lgamma(b);
        double sign = (std::tgamma(a) < 0.0) ? -1.0 : 1.0;
        double lv = lgb - lga + z + (a - b) * std::log(z);
        if (lv > 700.0)  return sign * 1e308;
        if (lv < -700.0) return 0.0;
        return sign * std::exp(lv);
    }

    // Large negative z — leading asymptotic:  Gamma(b)/Gamma(b-a) * |z|^{-a}
    if (z < -40.0) {
        double u = -z;
        double c = std::tgamma(b) / std::tgamma(b - a) * std::pow(u, -a);
        // One correction term
        double s = 1.0 - a * (a - b + 1.0) / u;
        return c * s;
    }

    // Power series — reliable for |z| ≤ 40
    double sum = 1.0, term = 1.0;
    for (int n = 1; n <= 1000; n++) {
        term *= (a + n - 1.0) * z / ((b + n - 1.0) * (double)n);
        sum  += term;
        if (!std::isfinite(sum))                               return 0.0;
        if (std::abs(term) < 1e-14 * (std::abs(sum) + 1e-30)) break;
    }
    return sum;
}

// =============================================================================
//  Parameters
// =============================================================================
struct Params {
    float gamma_v = 1.00f;
    float sigma   = 0.30f;
    float r0      = 0.05f;
    float T_mat   = 1.00f;
    float mu0     = 0.10f;
    float lambda  = 1.00f;
    float t_min   = 0.00f;
    float x_min   = -1.50f;   // default narrow to keep z in series range
    float x_max   =  1.50f;
    int   res     = 60;
    float clip_pct = 2.0f;    // percentile clip for colour normalisation (%)
    bool  auto_upd = true;
};

// =============================================================================
//  V(t, x)
// =============================================================================
static double compute_V(double t, double x, const Params& p) {
    double tau = (double)p.T_mat - t;
    if (tau < 1e-9) return 0.0;

    double sq  = std::sqrt(std::sqrt(2.0) * (double)p.sigma / M_PI);
    double ep  = std::exp(-0.5 * p.r0 * ((double)p.T_mat * p.T_mat - t * t));
    double tp  = std::pow(tau, 0.25);
    double g58 = std::tgamma(5.0 / 8.0);

    double drift = (std::abs((double)p.lambda) > 1e-9)
        ? ((double)p.mu0 / p.lambda) *
          (std::sin((double)p.lambda * p.T_mat) - std::sin((double)p.lambda * t))
        : (double)p.mu0 * tau;

    double z   = -(x + drift) / (2.0 * p.sigma * p.sigma * tau);
    double kum = kummer(-0.25, 0.5, z);

    double v = (double)p.gamma_v * sq * ep * tp * g58 * kum;
    return std::isfinite(v) ? v : 0.0;
}

// =============================================================================
//  Surface mesh
// =============================================================================
struct Vertex { float px, py, pz, r, g, b; };

struct Surface {
    std::vector<Vertex> verts;
    int   N        = 0;
    float vmin     = 0.f;   // display range (after percentile clip)
    float vmax     = 1.f;
    float v_raw_min = 0.f;  // actual min before clip
    float v_raw_max = 1.f;
    // Domain info (for axis labelling)
    float t0 = 0.f, t1 = 1.f;
    float x0 = -1.5f, x1 = 1.5f;
};

static void jet_cm(float t, float& r, float& g, float& b) {
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    r = 1.5f - std::abs(4.f * t - 3.f); r = r<0?0:r>1?1:r;
    g = 1.5f - std::abs(4.f * t - 2.f); g = g<0?0:g>1?1:g;
    b = 1.5f - std::abs(4.f * t - 1.f); b = b<0?0:b>1?1:b;
}

// build_surface runs on a background thread; progress_cb(0..100) called per row
static Surface build_surface(const Params& p,
                              std::function<void(int)> progress_cb = nullptr)
{
    Surface surf;
    int N = std::max(p.res, 2);
    surf.N  = N;
    surf.t0 = p.t_min;
    surf.t1 = p.T_mat * 0.99f;
    surf.x0 = p.x_min;
    surf.x1 = p.x_max;
    if (surf.t0 >= surf.t1) surf.t0 = 0.f;
    surf.verts.resize((size_t)N * N);

    // First pass — compute V
    std::vector<float> vals((size_t)N * N);
    float raw_min =  FLT_MAX;
    float raw_max = -FLT_MAX;

    for (int i = 0; i < N; i++) {
        double t = surf.t0 + (surf.t1 - surf.t0) * (double)i / (N - 1);
        for (int j = 0; j < N; j++) {
            double x = surf.x0 + (surf.x1 - surf.x0) * (double)j / (N - 1);
            float  v = (float)compute_V(t, x, p);
            vals[(size_t)i * N + j] = v;
            if (v < raw_min) raw_min = v;
            if (v > raw_max) raw_max = v;
        }
        if (progress_cb) progress_cb((i + 1) * 70 / N); // 0..70 for row pass
    }
    surf.v_raw_min = raw_min;
    surf.v_raw_max = raw_max;

    // Percentile clip
    float clip_lo, clip_hi;
    {
        std::vector<float> sorted = vals;
        // filter non-finite just in case
        sorted.erase(std::remove_if(sorted.begin(), sorted.end(),
            [](float v){ return !std::isfinite(v); }), sorted.end());
        if (sorted.empty()) { clip_lo = 0.f; clip_hi = 1.f; }
        else {
            std::sort(sorted.begin(), sorted.end());
            float pct = p.clip_pct / 100.f;
            pct = pct < 0.f ? 0.f : pct > 0.49f ? 0.49f : pct;
            int lo_i = (int)(pct * (sorted.size() - 1));
            int hi_i = (int)((1.f - pct) * (sorted.size() - 1));
            clip_lo = sorted[lo_i];
            clip_hi = sorted[hi_i];
            if (clip_hi - clip_lo < 1e-10f) { clip_lo = sorted.front(); clip_hi = sorted.back(); }
            if (clip_hi - clip_lo < 1e-10f) { clip_lo -= 0.5f; clip_hi += 0.5f; }
        }
    }
    surf.vmin = clip_lo;
    surf.vmax = clip_hi;
    float vrange = clip_hi - clip_lo;

    float t_mid = (surf.t0 + surf.t1) * 0.5f;
    float t_sc  = 2.f / std::max(surf.t1 - surf.t0, 1e-6f);
    float x_mid = (surf.x0 + surf.x1) * 0.5f;
    float x_sc  = 2.f / std::max(surf.x1 - surf.x0, 1e-6f);

    // Second pass — build vertices
    for (int i = 0; i < N; i++) {
        float t = surf.t0 + (surf.t1 - surf.t0) * (float)i / (N - 1);
        for (int j = 0; j < N; j++) {
            float x  = surf.x0 + (surf.x1 - surf.x0) * (float)j / (N - 1);
            float v  = vals[(size_t)i * N + j];
            float vc = v < clip_lo ? clip_lo : v > clip_hi ? clip_hi : v;
            float tn = (vc - clip_lo) / vrange;

            auto& vt = surf.verts[(size_t)i * N + j];
            vt.px = (t - t_mid) * t_sc;
            vt.py = tn * 1.5f - 0.75f;
            vt.pz = (x - x_mid) * x_sc;
            jet_cm(tn, vt.r, vt.g, vt.b);
        }
        if (progress_cb) progress_cb(70 + (i + 1) * 30 / N); // 70..100 for vertex pass
    }
    return surf;
}

// =============================================================================
//  Global render state
// =============================================================================
static float g_rotX  = 22.f, g_rotY = -35.f;
static float g_ortho = 1.8f;   // orthographic half-height (zoom via this)
static bool  g_drag  = false;
static double g_lx = 0, g_ly = 0;

// Matrices read back after setting up the camera (for label projection)
static float g_mv[16], g_proj[16];
static int   g_vp[4];   // {vx, vy, vw, vh}

// =============================================================================
//  GLFW callbacks
// =============================================================================
static void cb_mouse(GLFWwindow* w, int btn, int act, int) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (btn == GLFW_MOUSE_BUTTON_LEFT) {
        g_drag = (act == GLFW_PRESS);
        glfwGetCursorPos(w, &g_lx, &g_ly);
    }
}
static void cb_cursor(GLFWwindow*, double x, double y) {
    if (!g_drag) return;
    g_rotY += (float)(x - g_lx) * 0.35f;
    g_rotX += (float)(y - g_ly) * 0.35f;
    g_lx = x; g_ly = y;
}
static void cb_scroll(GLFWwindow*, double, double dy) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    g_ortho *= std::exp(-0.08f * (float)dy);
    if (g_ortho < 0.3f) g_ortho = 0.3f;
    if (g_ortho > 8.f)  g_ortho = 8.f;
}

// =============================================================================
//  3D → 2D screen projection  (uses g_mv, g_proj, g_vp)
// =============================================================================
static ImVec2 project(float wx, float wy, float wz) {
    // Apply model-view
    float ex = g_mv[0]*wx + g_mv[4]*wy + g_mv[8]*wz  + g_mv[12];
    float ey = g_mv[1]*wx + g_mv[5]*wy + g_mv[9]*wz  + g_mv[13];
    float ez = g_mv[2]*wx + g_mv[6]*wy + g_mv[10]*wz + g_mv[14];
    float ew = g_mv[3]*wx + g_mv[7]*wy + g_mv[11]*wz + g_mv[15];
    // Apply projection
    float cx = g_proj[0]*ex + g_proj[4]*ey + g_proj[8]*ez  + g_proj[12]*ew;
    float cy = g_proj[1]*ex + g_proj[5]*ey + g_proj[9]*ez  + g_proj[13]*ew;
    float cw = g_proj[3]*ex + g_proj[7]*ey + g_proj[11]*ez + g_proj[15]*ew;
    if (std::abs(cw) < 1e-6f) return {-9999.f, -9999.f};
    float ndcx = cx / cw, ndcy = cy / cw;
    float sx = g_vp[0] + (ndcx + 1.f) * 0.5f * g_vp[2];
    float sy = g_vp[1] + (1.f - (ndcy + 1.f) * 0.5f) * g_vp[3]; // Y-flip
    return {sx, sy};
}

// =============================================================================
//  Draw helpers
// =============================================================================
static void draw_floor_grid(float y, float sz = 1.15f, int D = 8) {
    glColor3f(0.12f, 0.12f, 0.18f);
    glLineWidth(0.7f);
    float step = 2.f * sz / D;
    glBegin(GL_LINES);
    for (int i = 0; i <= D; i++) {
        float p = -sz + i * step;
        glVertex3f(p, y, -sz); glVertex3f(p, y, sz);
        glVertex3f(-sz, y, p); glVertex3f(sz, y, p);
    }
    glEnd();
}

// Draw one arrowhead at (tx,ty,tz) pointing in +axis direction
// perp1/perp2 define the two perpendicular directions for the 4-line arrow
static void arrow_head(float tx, float ty, float tz,
                        float p1x, float p1y, float p1z,
                        float p2x, float p2y, float p2z,
                        float back, float spread) {
    float a1x = tx - back*(1.f) + spread*p1x;
    float a1y = ty              + spread*p1y;
    float a1z = tz - back*(0.f) + spread*p1z;
    // Use the direction vectors properly:
    (void)a1x; (void)a1y; (void)a1z;

    glVertex3f(tx, ty, tz);
    glVertex3f(tx - back + spread*p1x, ty + spread*p1y, tz + spread*p1z);
    glVertex3f(tx, ty, tz);
    glVertex3f(tx - back - spread*p1x, ty - spread*p1y, tz - spread*p1z);
    glVertex3f(tx, ty, tz);
    glVertex3f(tx - back + spread*p2x, ty + spread*p2y, tz + spread*p2z);
    glVertex3f(tx, ty, tz);
    glVertex3f(tx - back - spread*p2x, ty - spread*p2y, tz - spread*p2z);
}

static void draw_axes_and_labels(const Surface& surf) {
    const float YB   = -0.75f;    // floor Y
    const float L    =  1.20f;    // half-length of axis lines
    const float A    =  0.09f;    // arrowhead back-step
    const float S    =  0.045f;   // arrowhead spread

    glLineWidth(2.2f);
    glBegin(GL_LINES);

    // t-axis  (world X, red)
    glColor3f(1.f, 0.30f, 0.30f);
    glVertex3f(-L, YB, 0); glVertex3f(L, YB, 0);
    arrow_head(L, YB, 0,   0,1,0,  0,0,1,   A, S);

    // V-axis  (world Y, green)
    glColor3f(0.30f, 1.f, 0.30f);
    glVertex3f(0, YB, 0); glVertex3f(0, YB+1.5f, 0);
    arrow_head(0, YB+1.5f, 0,  1,0,0,  0,0,1,  A, S);

    // x-axis  (world Z, blue)
    glColor3f(0.30f, 0.30f, 1.f);
    glVertex3f(0, YB, -L); glVertex3f(0, YB, L);
    arrow_head(0, YB, L,   1,0,0,  0,1,0,  A, S);

    glEnd();

    // --- Tick marks ---
    const int NTICKS = 5;
    glLineWidth(1.2f);
    glBegin(GL_LINES);

    // t-axis ticks (at Y=YB, Z=0)
    glColor3f(1.f, 0.55f, 0.55f);
    for (int k = 0; k <= NTICKS; k++) {
        float px = -L + 2.f * L * k / NTICKS;
        glVertex3f(px, YB - 0.04f, 0); glVertex3f(px, YB + 0.04f, 0);
    }

    // V-axis ticks (at X=0, Z=0)
    glColor3f(0.55f, 1.f, 0.55f);
    for (int k = 0; k <= NTICKS; k++) {
        float py = YB + 1.5f * k / NTICKS;
        glVertex3f(-0.04f, py, 0); glVertex3f(0.04f, py, 0);
    }

    // x-axis ticks (at X=0, Y=YB)
    glColor3f(0.55f, 0.55f, 1.f);
    for (int k = 0; k <= NTICKS; k++) {
        float pz = -L + 2.f * L * k / NTICKS;
        glVertex3f(0, YB - 0.04f, pz); glVertex3f(0, YB + 0.04f, pz);
    }

    glEnd();
}

static void draw_surface(const Surface& surf) {
    if (surf.N < 2) return;
    int N = surf.N;

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.f, 1.f);

    for (int i = 0; i < N - 1; i++) {
        glBegin(GL_TRIANGLE_STRIP);
        for (int j = 0; j < N; j++) {
            for (int d = 0; d <= 1; d++) {
                const auto& v = surf.verts[(size_t)(i + d) * N + j];
                glColor3f(v.r, v.g, v.b);
                glVertex3f(v.px, v.py, v.pz);
            }
        }
        glEnd();
    }
    glDisable(GL_POLYGON_OFFSET_FILL);

    // Sparse wireframe
    int step = std::max(1, N / 12);
    glColor3f(0.06f, 0.06f, 0.08f);
    glLineWidth(0.5f);
    for (int i = 0; i < N; i += step) {
        glBegin(GL_LINE_STRIP);
        for (int j = 0; j < N; j++) {
            const auto& v = surf.verts[(size_t)i * N + j];
            glVertex3f(v.px, v.py, v.pz);
        }
        glEnd();
    }
    for (int j = 0; j < N; j += step) {
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i < N; i++) {
            const auto& v = surf.verts[(size_t)i * N + j];
            glVertex3f(v.px, v.py, v.pz);
        }
        glEnd();
    }
}

// Draw projected axis labels via ImGui foreground draw-list
static void draw_axis_labels(const Surface& surf) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float YB = -0.75f;
    const float L  =  1.20f;
    const int   NTICKS = 5;
    const ImU32 COL_T = IM_COL32(255, 140, 140, 220);
    const ImU32 COL_V = IM_COL32(140, 255, 140, 220);
    const ImU32 COL_X = IM_COL32(140, 140, 255, 220);

    char buf[32];

    // --- t-axis labels ---
    for (int k = 0; k <= NTICKS; k++) {
        float frac = (float)k / NTICKS;
        float px   = -L + 2.f * L * frac;
        float tval = surf.t0 + (surf.t1 - surf.t0) * frac;
        ImVec2 sc  = project(px, YB - 0.10f, 0.f);
        if (sc.x > 0) {
            std::snprintf(buf, sizeof(buf), "%.2f", tval);
            dl->AddText({sc.x - 12.f, sc.y}, COL_T, buf);
        }
    }
    // axis name
    {
        ImVec2 sc = project(L + 0.08f, YB, 0.f);
        if (sc.x > 0) dl->AddText({sc.x, sc.y - 7.f}, COL_T, "t");
    }

    // --- V-axis labels ---
    for (int k = 0; k <= NTICKS; k++) {
        float frac = (float)k / NTICKS;
        float py   = YB + 1.5f * frac;
        float vval = surf.vmin + (surf.vmax - surf.vmin) * frac;
        ImVec2 sc  = project(-0.06f, py, 0.f);
        if (sc.x > 0) {
            std::snprintf(buf, sizeof(buf), "%.3f", vval);
            dl->AddText({sc.x - 36.f, sc.y - 6.f}, COL_V, buf);
        }
    }
    {
        ImVec2 sc = project(0.f, YB + 1.55f, 0.f);
        if (sc.x > 0) dl->AddText({sc.x + 4.f, sc.y}, COL_V, "V");
    }

    // --- x-axis labels ---
    for (int k = 0; k <= NTICKS; k++) {
        float frac = (float)k / NTICKS;
        float pz   = -L + 2.f * L * frac;
        float xval = surf.x0 + (surf.x1 - surf.x0) * frac;
        ImVec2 sc  = project(0.f, YB - 0.10f, pz);
        if (sc.x > 0) {
            std::snprintf(buf, sizeof(buf), "%.2f", xval);
            dl->AddText({sc.x - 12.f, sc.y}, COL_X, buf);
        }
    }
    {
        ImVec2 sc = project(0.f, YB, L + 0.08f);
        if (sc.x > 0) dl->AddText({sc.x, sc.y - 7.f}, COL_X, "x");
    }
}

// =============================================================================
//  Background compute thread
// =============================================================================
static std::thread        g_worker;
static std::atomic<int>   g_progress{0};    // 0–100
static std::atomic<bool>  g_computing{false};
static std::atomic<bool>  g_result_ready{false};
static Surface            g_pending;
static std::mutex         g_pending_mtx;
static bool               g_dirty = true;  // params changed
static Params             g_queued;        // latest params to compute

static void start_compute(const Params& p) {
    if (g_computing.load()) return;       // drop — result will come soon
    if (g_worker.joinable()) g_worker.join();
    g_computing     = true;
    g_result_ready  = false;
    g_progress      = 0;
    g_queued        = p;
    g_worker = std::thread([p]() {
        Surface s = build_surface(p, [](int prog){ g_progress.store(prog); });
        {
            std::lock_guard<std::mutex> lk(g_pending_mtx);
            g_pending = std::move(s);
        }
        g_progress     = 100;
        g_result_ready = true;
        g_computing    = false;
    });
}

// =============================================================================
//  Colourbar helper
// =============================================================================
static void draw_colorbar(float vmin, float vmax, float width) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();
    float       h   = 16.f;
    int         seg = (int)width;
    for (int i = 0; i < seg; i++) {
        float t  = (float)i / (seg - 1);
        float r, g, b;
        jet_cm(t, r, g, b);
        ImU32 c = IM_COL32((int)(r*255),(int)(g*255),(int)(b*255), 255);
        dl->AddRectFilled({pos.x + width * i/seg, pos.y},
                          {pos.x + width * (i+1)/seg, pos.y + h}, c);
    }
    dl->AddRect({pos.x, pos.y}, {pos.x+width, pos.y+h}, IM_COL32(70,70,90,200));
    ImGui::Dummy({width, h});
}

// =============================================================================
//  main
// =============================================================================
int main() {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

    GLFWwindow* win = glfwCreateWindow(1440, 900,
        "V(t,x) — Equation Surface", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    glfwSetMouseButtonCallback(win, cb_mouse);
    glfwSetCursorPosCallback(win, cb_cursor);
    glfwSetScrollCallback(win, cb_scroll);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiStyle& sty = ImGui::GetStyle();
    sty.WindowRounding = 7.f; sty.FrameRounding = 5.f;
    sty.GrabRounding   = 5.f; sty.WindowBorderSize = 0.f;
    auto& C = sty.Colors;
    C[ImGuiCol_WindowBg]         = {0.06f,0.06f,0.09f,0.97f};
    C[ImGuiCol_FrameBg]          = {0.11f,0.11f,0.16f,1.00f};
    C[ImGuiCol_FrameBgHovered]   = {0.17f,0.17f,0.24f,1.00f};
    C[ImGuiCol_SliderGrab]       = {0.22f,0.53f,0.95f,1.00f};
    C[ImGuiCol_SliderGrabActive] = {0.40f,0.70f,1.00f,1.00f};
    C[ImGuiCol_Button]           = {0.18f,0.28f,0.52f,1.00f};
    C[ImGuiCol_ButtonHovered]    = {0.28f,0.42f,0.72f,1.00f};
    C[ImGuiCol_Separator]        = {0.22f,0.22f,0.32f,1.00f};

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL2_Init();

    Params params;
    Surface surf = build_surface(params);   // initial synchronous build
    g_dirty = false;

    const double GAMMA_5_8 = std::tgamma(5.0 / 8.0);
    const int PANEL = 370;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        // --- Swap in completed surface ---
        if (g_result_ready.load()) {
            if (g_worker.joinable()) g_worker.join();
            {
                std::lock_guard<std::mutex> lk(g_pending_mtx);
                surf = std::move(g_pending);
            }
            g_result_ready = false;
            // If params changed again while computing, restart
            if (g_dirty && params.auto_upd) {
                g_dirty = false;
                start_compute(params);
            }
        }

        // Kick off compute if needed and not already running
        if (g_dirty && params.auto_upd && !g_computing.load()) {
            g_dirty = false;
            start_compute(params);
        }

        int W, H;
        glfwGetFramebufferSize(win, &W, &H);
        const int VX = PANEL, VW = W - PANEL;

        // ---- Clear ----
        glViewport(0, 0, W, H);
        glClearColor(0.03f, 0.03f, 0.05f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // ---- 3-D view ----
        glViewport(VX, 0, VW, H);
        g_vp[0] = VX; g_vp[1] = 0; g_vp[2] = VW; g_vp[3] = H;

        glEnable(GL_DEPTH_TEST);
        float aspect = VW > 0 && H > 0 ? (float)VW / (float)H : 1.f;
        float oh = g_ortho, ow = oh * aspect;

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-ow, ow, -oh, oh, -50.0, 50.0);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.f, -0.1f, -5.f);   // slight downward offset + depth
        glRotatef(g_rotX, 1.f, 0.f, 0.f);
        glRotatef(g_rotY, 0.f, 1.f, 0.f);

        // Read back matrices for label projection
        glGetFloatv(GL_MODELVIEW_MATRIX,  g_mv);
        glGetFloatv(GL_PROJECTION_MATRIX, g_proj);

        draw_floor_grid(-0.75f);
        draw_axes_and_labels(surf);
        draw_surface(surf);

        // Restore
        glViewport(0, 0, W, H);
        glDisable(GL_DEPTH_TEST);

        // ---- ImGui ----
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Panel
        ImGui::SetNextWindowPos({0,0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({(float)PANEL,(float)H}, ImGuiCond_Always);
        ImGui::Begin("##p", nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Title
        ImGui::PushStyleColor(ImGuiCol_Text,{0.95f,0.82f,0.28f,1.f});
        ImGui::SetWindowFontScale(1.12f);
        ImGui::Text("  V(t,x) Surface Visualizer");
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();

        // Equation
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,{0.68f,0.92f,0.60f,1.f});
        ImGui::TextWrapped("V = g*sqrt(sqrt(2)*s/pi)");
        ImGui::TextWrapped("  * exp(-r0/2*(T^2-t^2))");
        ImGui::TextWrapped("  * (T-t)^(1/4) * Gamma(5/8)");
        ImGui::TextWrapped("  * 1F1(-1/4 ; 1/2 ; -z)");
        ImGui::PushStyleColor(ImGuiCol_Text,{0.55f,0.80f,0.50f,1.f});
        ImGui::TextWrapped("z=[x+(m/L)*(sin(L*T)-sin(L*t))]/(2s^2*(T-t))");
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text,{0.42f,0.42f,0.55f,1.f});
        ImGui::Text("  Gamma(5/8) = %.7f", GAMMA_5_8);
        ImGui::PopStyleColor();

        ImGui::Spacing(); ImGui::Separator();

        // ---- Parameters ----
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,{0.52f,0.82f,1.f,1.f});
        ImGui::Text("  Parameters");
        ImGui::PopStyleColor();
        ImGui::Spacing();

#define SLI(lbl,fld,lo,hi,fmt) \
        if (ImGui::SliderFloat(lbl, &params.fld, lo, hi, fmt)) g_dirty=true;
        SLI("g  (gamma)",    gamma_v,  0.001f, 20.f,  "%.4f")
        SLI("s  (sigma)",    sigma,    0.005f,  2.0f, "%.4f")
        SLI("r0",            r0,      -0.5f,   2.0f,  "%.4f")
        SLI("T  (maturity)", T_mat,    0.05f,  10.f,  "%.3f")
        SLI("m  (mu0)",      mu0,    -10.f,   10.f,   "%.3f")
        SLI("L  (lambda)",   lambda,   0.001f, 20.f,  "%.4f")
#undef SLI

        ImGui::Spacing(); ImGui::Separator();

        // ---- Domain ----
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,{0.52f,0.82f,1.f,1.f});
        ImGui::Text("  Domain");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        {
            float tmax = params.T_mat * 0.90f;
            if (params.t_min > tmax) params.t_min = tmax;
            if (ImGui::SliderFloat("t_min", &params.t_min, 0.f, tmax, "%.3f"))
                g_dirty = true;
        }
        if (ImGui::SliderFloat("x_min", &params.x_min, -15.f, 0.f,  "%.2f")) g_dirty=true;
        if (ImGui::SliderFloat("x_max", &params.x_max,   0.f, 15.f, "%.2f")) g_dirty=true;
        if (ImGui::SliderFloat("clip %", &params.clip_pct, 0.f, 10.f, "%.1f%%"))
            g_dirty = true;

        ImGui::Spacing(); ImGui::Separator();

        // ---- Resolution ----
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,{0.52f,0.82f,1.f,1.f});
        ImGui::Text("  Resolution");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (ImGui::SliderInt("Grid N", &params.res, 10, 300) && params.auto_upd)
            g_dirty = true;
        ImGui::Checkbox("Auto-update", &params.auto_upd);
        ImGui::SameLine();
        if (ImGui::Button("Rebuild")) {
            if (!g_computing.load()) {
                g_dirty = false;
                start_compute(params);
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text,{0.40f,0.40f,0.54f,1.f});
        ImGui::TextWrapped("Low=10  Med=60  High=150  Ultra=300");
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // ---- Compute progress bar ----
        if (g_computing.load()) {
            float prog = g_progress.load() / 100.f;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,{0.22f,0.62f,1.f,1.f});
            ImGui::ProgressBar(prog, {-1,0}, "Computing...");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,{0.18f,0.60f,0.25f,1.f});
            ImGui::ProgressBar(1.f, {-1,0}, "Ready");
            ImGui::PopStyleColor();
        }

        ImGui::Spacing(); ImGui::Separator();

        // ---- Camera ----
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,{0.52f,0.82f,1.f,1.f});
        ImGui::Text("  Camera");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::SliderFloat("Rot X",    &g_rotX,  -89.f, 89.f,  "%.1f");
        ImGui::SliderFloat("Rot Y",    &g_rotY, -180.f, 180.f, "%.1f");
        ImGui::SliderFloat("Ortho zoom", &g_ortho, 0.3f, 8.f,  "%.2f");
        if (ImGui::Button("Reset Camera")) {
            g_rotX=22.f; g_rotY=-35.f; g_ortho=1.8f;
        }
        ImGui::PushStyleColor(ImGuiCol_Text,{0.38f,0.38f,0.52f,1.f});
        ImGui::Text("Drag: rotate   Scroll: zoom");
        ImGui::PopStyleColor();

        ImGui::Spacing(); ImGui::Separator();

        // ---- Output info + colorbar ----
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,{0.52f,0.82f,1.f,1.f});
        ImGui::Text("  Output");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        {
            float avail = ImGui::GetContentRegionAvail().x - 6.f;
            draw_colorbar(surf.vmin, surf.vmax, avail);
            ImGui::PushStyleColor(ImGuiCol_Text,{0.40f,0.40f,0.54f,1.f});
            ImGui::Text("%.4f", surf.vmin);
            ImGui::SameLine((float)(PANEL - 90));
            ImGui::Text("%.4f", surf.vmax);
            ImGui::Spacing();
            ImGui::Text("Grid: %d x %d", surf.N, surf.N);
            ImGui::Text("Raw V: [%.3f, %.3f]", surf.v_raw_min, surf.v_raw_max);
            ImGui::Text("Axes:  red=t  green=V  blue=x");
            ImGui::PopStyleColor();
        }

        ImGui::End();

        // Projected axis labels (drawn over everything via foreground list)
        draw_axis_labels(surf);

        ImGui::Render();
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    if (g_computing.load()) {
        // Let thread finish before exit
        if (g_worker.joinable()) g_worker.join();
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
