# CPP_projects — CLAUDE.md

Working directory: `C:\Users\vedan\OneDrive\Desktop\Projects\CPP_projects`

## What this project is

A real-time C++ / OpenGL desktop visualizer for the stochastic finance surface equation:

```
V(t,x) = γ √(√2·σ/π) · exp(-r₀/2·(T²-t²)) · (T-t)^(1/4) · Γ(3/4) · ₁F₁(-¼; ½; -z)

z = [x + (μ₀/λ)·(sin(λT)−sin(λt))] / (2σ²(T−t))
```

Parameters: γ (gamma), σ (sigma), r₀, T (maturity), μ₀ (mu0), λ (lambda)

## Files

| File | Purpose |
|------|---------|
| `V_surface.cpp` | Main source — all math, rendering, UI, and export in one file |
| `CMakeLists.txt` | Build system — auto-fetches GLFW3 and Dear ImGui via FetchContent |
| `build/V_surface.exe` | Compiled binary (Windows, MinGW) |

## Dependencies

Auto-fetched by CMake on first build (requires internet):
- **GLFW 3.4** — window + input
- **Dear ImGui v1.91.6** — UI panel and widgets (OpenGL2 backend)
- **OpenGL 2.1** — 3D surface rendering (legacy immediate mode)

System requirements:
- **MinGW g++ 13** at `C:\msys64\ucrt64\bin\g++.exe`
- **CMake ≥ 4.0**
- Windows 10/11 (GDI used for 4K text overlay in export)

## Build

```powershell
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER="C:/msys64/ucrt64/bin/gcc.exe" `
      -DCMAKE_CXX_COMPILER="C:/msys64/ucrt64/bin/g++.exe"
cmake --build build --config Release -j4
```

First build downloads GLFW + ImGui (~30s). Subsequent builds are seconds.

## Run

Always launch with an explicit working directory so exports land in the project folder:

```powershell
Start-Process -WorkingDirectory "C:\Users\vedan\OneDrive\Desktop\Projects\CPP_projects" `
  "C:\Users\vedan\OneDrive\Desktop\Projects\CPP_projects\build\V_surface.exe"
```

Or from the project directory:
```powershell
.\build\V_surface.exe
```

## Key implementation details

**Math (`V_surface.cpp`)**
- `kummer(a, b, z)` — Confluent hypergeometric ₁F₁ via power series for |z| ≤ 15, asymptotic expansion for z > 15. Critical: threshold must stay at 15 — series diverges catastrophically above it.
- Default `x ∈ [-1.5, 1.5]` keeps `z` in the series-convergent range.
- Γ(3/4) ≈ 1.2254167 — corrected from prior Γ(5/8) in this session.
- Percentile-based colour normalisation (default 2%) clips outlier values for the jet colourmap.

**Params struct fields**

| Field | Default | Purpose |
|-------|---------|---------|
| `gamma_v` | 1.0 | amplitude |
| `sigma` | 0.3 | volatility |
| `r0` | 0.05 | discount rate |
| `T_mat` | 1.0 | maturity |
| `mu0` | 0.1 | drift amplitude |
| `lambda` | 1.0 | drift frequency |
| `t_min` | 0.0 | time domain start |
| `x_min` | -1.5 | spatial domain start |
| `x_max` | 1.5 | spatial domain end |
| `res` | 60 | grid resolution N |
| `clip_pct` | 2.0 | percentile clip % |
| `auto_upd` | true | auto-rebuild on change |
| `force_pos` | false | take \|V\| everywhere |
| `pos_domain` | false | lock x_min = 0 (t,x ≥ 0) |
| `zoom_domain` | false | scale x domain with ortho zoom |

**Rendering**
- Orthographic projection (`glOrtho`) — no perspective distortion
- `draw_surface()` — triangle strips + sparse wireframe overlay
- `draw_axes_and_labels()` — 4-line arrowheads + 5 tick marks per axis
- `draw_axis_labels()` — projects 3D ticks to 2D via MVP readback, draws ImGui foreground text
- `draw_floor_grid()` — reference grid at y = -0.75

**Threading**
- `build_surface()` runs on a `std::thread`; `g_progress` (atomic int 0–100) drives the ImGui ProgressBar
- Main thread swaps surface when `g_result_ready` fires; re-queues if params changed during compute
- Uncheck Auto-update before dragging high-res sliders (N > 150)

**4K Export**
- FBO loaded at runtime via `glfwGetProcAddress` (tries core GL3 names first, falls back to `EXT` suffix)
- Renders scene offscreen at 3840×2160 into a texture-backed FBO
- `overlay_text_gdi()` — copies pixels into a GDI DIB, draws ClearType axis tick labels (Arial) and parameter info box (Consolas) using the same 3D→2D projection as the live view, copies back
- Saves `V_surface_YYYYMMDD_HHMMSS.bmp` (lossless ~24 MB) + matching `.csv` (t, x, V columns + param header)
- Files land in the process working directory

## Controls

| Input | Action |
|-------|--------|
| Left-click drag | Rotate surface |
| Scroll wheel | Zoom (adjusts `g_ortho`) |
| Sliders | Drag to sweep; **type exact value** in the input box to the right |
| "Rebuild" button | Manual trigger (use when auto-update off) |
| "Reset Camera" | Restores rotX=22, rotY=-35, ortho=1.8 |
| Export 4K BMP + CSV | Renders 3840×2160 with axis labels + params baked in |

## Domain checkboxes (Domain section)

| Checkbox | Effect |
|----------|--------|
| Scale domain with zoom | x_min/x_max expand proportionally with `g_ortho` |
| t, x positive only | Locks x_min = 0; rebuilds to positive quadrant |
| \|V\| (force positive) | Takes absolute value of every V before rendering |

## Obsidian vault

Located at: `C:\Users\vedan\OneDrive\Desktop\Projects\CPP_projects\obsidian vault`
