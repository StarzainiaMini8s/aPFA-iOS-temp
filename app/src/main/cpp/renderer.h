// renderer.h — shared renderer interface + per-instance data layouts.
//
// aPFA ships two renderer implementations behind the IRenderer interface:
//   • RendererES2 (renderer.cpp / renderer_es2.h) — OpenGL ES 2.0 baseline, GLSL
//     ES 1.00, runs on ES2-only GPUs (Mali-400 / MT6570 class). This is also the
//     iOS renderer. The "Legacy Renderer (GLES 2.0)" toggle selects it.
//   • RendererES3 (renderer_es3.cpp / renderer_es3.h) — OpenGL ES 3.0, VAOs,
//     GLSL ES 3.00. The default on Android. Android-only (not built for iOS).
//
// createRenderer() picks one at runtime. The engine only ever sees IRenderer.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace apfa {

// Per-instance note record the engine produces. 28 bytes — keep this layout
// (and order) in lockstep with RendererES3's hardcoded note-VAO offsets
// (renderer_es3.cpp, stride 28). RendererES2 copies this into its own
// keyX/keyW-augmented NoteInstanceES2 (renderer_es2.h) at draw time, because
// GLSL ES 1.00 cannot dynamically index a per-key uniform array.
struct NoteInstance {
    float    startSec;
    float    durSec;
    float    key;          // MIDI key 0..127
    uint32_t colorPrimary;
    uint32_t colorDark;
    uint32_t colorVeryDark;
    uint32_t isSharp;
};

struct RectInstance {
    float    x, y, w, h;   // normalised [0..1], y=bottom in GL
    uint32_t color;         // RGBA8
};

// Gradient rect: 4 corner colours. Layout must be 32 bytes.
// y = GL bottom of rect (y_screen_top = 1 - y - h).
struct GradInstance {
    float    x, y, w, h;   // normalised, y=GL bottom
    uint32_t cTL, cTR, cBR, cBL;  // 4 bytes each = 16 bytes total → 32 bytes total
};

// Arbitrary skew quad (4 corners in normalised screen space).
// Corners: TL(x,y), TR(x,y), BR(x,y), BL(x,y) + per-corner colors.
// Total = 8*4 + 4*4 = 32+16 = 48 bytes.
struct SkewInstance {
    float    tlX, tlY;   // top-left (screen: high y in GL)
    float    trX, trY;   // top-right
    float    brX, brY;   // bottom-right
    float    blX, blY;   // bottom-left
    uint32_t cTL, cTR, cBR, cBL;
};

struct GlyphInstance {
    float    x, y;
    float    u;
    uint32_t color;
};

// Renderer interface. `window` is the opaque native surface handle:
// ANativeWindow* on Android, CAEAGLLayer* on iOS.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual bool initEGL(void* window) = 0;
    virtual void resize(int w, int h) = 0;
    virtual void setSwapInterval(bool vsync) = 0;
    virtual void setNoteRange(int startNote, int endNote) = 0;
    virtual void setBgColor(uint32_t bgrColor) = 0;
    // Upload a stretched RGBA background image (call on the render thread). rgba is
    // tightly packed w*h*4 bytes, row 0 = top. Pass null/0 to drop back to the
    // solid bgColor fill.
    virtual void uploadBgImage(const uint8_t* rgba, int w, int h) = 0;
    virtual void destroyEGL() = 0;
    virtual bool valid() const = 0;

    virtual void render(float clockSec, float totalSec, float fps,
                        float windowSec,
                        const std::vector<NoteInstance>& notes,
                        const uint32_t keyColor[128]) = 0;
};

// Factory. legacy==true selects RendererES2 (the "Legacy Renderer" toggle);
// legacy==false selects RendererES3 on Android. iOS always returns RendererES2.
std::unique_ptr<IRenderer> createRenderer(bool legacy);

}  // namespace apfa
