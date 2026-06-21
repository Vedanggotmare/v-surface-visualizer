# CPP_projects — CLAUDE.md

Working directory: `C:\Users\vedan\OneDrive\Desktop\Projects\CPP_projects`

## What this project is

A real-time C++ / OpenGL desktop visualizer for the stochastic finance surface equation:

```
V(t,x) = γ √(√2·σ/π) · exp(-r₀/2·(T²-t²)) · (T-t)^(1/4) · Γ(5/8) · ₁F₁(-¼; ½; -z)

z = [x + (μ₀/λ)·(sin(λT)−sin(λt))] / (2σ²(T−t))
```

Parameters: γ (gamma), σ (sigma), r₀, T (maturity), μ₀ (mu0), λ (lambda)

## Files

| File | Purpose |
|------|---------|
| `V_surface.cpp` | Main source — all math, rendering, and UI in one file |
| `CMakeLists.txt` | Build system — auto-fetches GLFW3 and Dear ImGui via FetchContent |
| `build/V_surface.exe` | Compiled binary (Windows, MinGW) |

## Dependencies

Auto-fetched by CMake on first build (requires internet):
- **GLFW 3.4** — window + input
- **Dear ImGui v1.91.6** — UI panel and widgets (OpenGL2 backend)
- **OpenGL 2.1** — 3D surface rendering (legacy immediate mode)

System requirement:
- **MinGW g++ 13** at `C:\msys64\ucrt64\bin\g++.exe`
- **CMake ≥ 4.0**

## Build

```powershell
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER="C:/msys64/ucrt64/bin/gcc.exe" `
      -DCMAKE_CXX_COMPILER="C:/msys64/ucrt64/bin/g++.exe"
cmake --build build --config Release -j4
```

First build downloads GLFW + ImGui (~30s). Subsequent builds are seconds.

## Run

```powershell
.\build\V_surface.exe
```

Or from anywhere:
```powershell
Start-Process "C:\Users\vedan\OneDrive\Desktop\Projects\CPP_projects\build\V_surface.exe"
```

## Key implementation details

**Math (`V_surface.cpp`)**
- `kummer(a, b, z)` — Confluent hypergeometric ₁F₁ via power series for |z| ≤ 15, asymptotic expansion otherwise. Critical: series diverges badly for z > 15 (catastrophic cancellation) — threshold must stay at 15, not higher.
- Default `x ∈ [-1.5, 1.5]` keeps `z` in the series-convergent range. Wider x ranges (e.g. x_min=-3) push z > 15 and produce exponentially large V values that collapse the colormap.
- Percentile-based colour normalisation (default 2%) clips outlier values so the jet colourmap shows the interesting structure even when edge values are extreme.

**Rendering**
- Orthographic projection (`glOrtho`) — no perspective distortion
- `draw_surface()` — triangle strips + sparse wireframe overlay
- `draw_axes_and_labels()` — 4-line arrowheads on each axis tip, 5 tick marks per axis
- `draw_axis_labels()` — projects 3D tick positions to 2D via `glGetFloatv(GL_MODELVIEW_MATRIX)` and draws ImGui foreground-list text
- `draw_floor_grid()` — reference grid at y = -0.75

**Threading**
- `build_surface()` runs on a `std::thread`; `g_progress` (atomic int 0–100) drives the ImGui `ProgressBar`
- Main thread swaps surface when `g_result_ready` fires; if params changed again during compute, queues a second run immediately
- `auto_upd` flag: uncheck before dragging high-res sliders (> N=150)

## Controls

| Input | Action |
|-------|--------|
| Left-click drag | Rotate surface |
| Scroll wheel | Zoom (adjusts `g_ortho`) |
| Sliders (panel) | All parameters update live |
| "Rebuild" button | Manual trigger (use when auto-update off) |
| "Reset Camera" | Restores rotX=22, rotY=-35, ortho=1.8 |

## Obsidian vault

Located at: `C:\Users\vedan\OneDrive\Desktop\Projects\CPP_projects\obsidian vault`
