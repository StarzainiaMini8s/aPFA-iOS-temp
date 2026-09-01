// renderer_es2.h — OpenGL ES 2.0 note field + piano keyboard (PFA-faithful) + bitmap HUD.
// Targets a baseline ES 2.0 context so the app runs on ES2-only GPUs (e.g. Mali-400
// class budget phones on Lollipop–Oreo). When an ES 3.0 context — or the ES2
// instanced-arrays extension — is available it is used as a fast path; otherwise
// instancing is emulated via CPU geometry expansion. See instancing layer below.
//
// This is also the iOS renderer (the iOS Xcode project builds renderer.cpp). On
// Android it is selected by the "Legacy Renderer (GLES 2.0)" toggle.
#pragma once

#include "renderer.h"

#include <cstdint>
#include <vector>

#if defined(__APPLE__)
  // iOS: OpenGL ES via the OpenGLES framework + EAGL. ES3/gl.h also exposes the
  // ES2 entry points, so the ES2-baseline renderer compiles unchanged; an ES3
  // context (chosen at init) lights up the native-instancing fast path. EGL does
  // not exist on iOS — context/surface live in eagl_surface.mm behind a void*.
  #include <OpenGLES/ES3/gl.h>
  #include <OpenGLES/ES3/glext.h>
#else
  #include <EGL/egl.h>
  #include <GLES2/gl2.h>
  #include <GLES2/gl2ext.h>
#endif

namespace apfa {

// RendererES2's private per-instance note layout (36 bytes). Same fields as the
// shared NoteInstance plus keyX/keyW, which the renderer fills from keyX_/keyW_
// because GLSL ES 1.00 cannot dynamically index a per-key uniform array. The
// kNoteAttrs table in renderer.cpp matches this exact field order/offsets.
struct NoteInstanceES2 {
    float    startSec;
    float    durSec;
    float    key;
    float    keyX;         // normalised x of the key's left edge
    float    keyW;         // normalised key width
    uint32_t colorPrimary;
    uint32_t colorDark;
    uint32_t colorVeryDark;
    uint32_t isSharp;
};

// Per-instance vertex attribute descriptor (location 0 is reserved for the unit-quad
// corner and handled internally by RendererES2::drawInstanced).
struct VAttr { GLuint loc; GLint size; GLenum type; GLboolean norm; GLuint offset; };

class RendererES2 : public IRenderer {
public:
    bool initEGL(void* window) override;
    void resize(int w, int h) override;
    void setSwapInterval(bool vsync) override;
    void setNoteRange(int startNote, int endNote) override;
    void setBgColor(uint32_t bgrColor) override { bgColor_ = bgrColor; }
    void uploadBgImage(const uint8_t* rgba, int w, int h) override;
    void destroyEGL() override;
#if defined(__APPLE__)
    bool valid() const override { return glSurface_ != nullptr; }
#else
    bool valid() const override { return context_ != EGL_NO_CONTEXT; }
#endif

    void render(float clockSec, float totalSec, float fps,
                float windowSec,
                const std::vector<NoteInstance>& notes,
                const uint32_t keyColor[128]) override;

private:
    bool buildPrograms();
    void buildFontTexture();
    void layoutKeyboard(int startNote, int endNote);

    // Keyboard rendering (PFA RenderKeys 1:1 port)
    void renderKeyboard(const uint32_t keyColor[128]);

    // Push helpers — pixel-space (y downward) → normalised GL coords
    void pushFlatRect(float x, float y, float cx, float cy, uint32_t c);
    void pushGradRect(float x, float y, float cx, float cy,
                      uint32_t cTL, uint32_t cTR, uint32_t cBR, uint32_t cBL);
    void pushSkew(float x1, float y1, float x2, float y2,
                  float x3, float y3, float x4, float y4,
                  uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4);

    void flushGrads();
    void flushSkews();

    void drawString(const char* str, float px, float py,
                    uint32_t color, float scale = 1.0f);
    void flushGlyphs();

    // ---- instancing abstraction ---------------------------------------------
    // Resolve native instancing entry points (ES3 core, or ES2 EXT/ANGLE/NV ext).
    // Leaves useNativeInstancing_ false when none are available (emulated path).
    void resolveInstancing(bool haveES3Context);

    // Draw `count` instances of a unit quad through `prog`, reading per-instance
    // data from `data` (count*stride bytes) uploaded into `instVbo`. Uses native
    // instancing when available, otherwise expands to 6 verts/instance on the CPU.
    void drawInstanced(GLuint prog, const void* data, int count, GLsizei stride,
                       const VAttr* attrs, int nattrs, GLuint instVbo);

    // Ensure cornerVbo_ holds the 6-corner triangle pattern repeated `count` times
    // (emulated path only).
    void ensureCorners(int count);

#if defined(__APPLE__)
    void*          glSurface_ = nullptr;   // opaque EAGL surface (eagl_surface.mm)
#else
    EGLDisplay     display_ = EGL_NO_DISPLAY;
    EGLSurface     surface_ = EGL_NO_SURFACE;
    EGLContext     context_ = EGL_NO_CONTEXT;
#endif
    void*          window_  = nullptr;     // ANativeWindow* (Android) / CAEAGLLayer* (iOS)
    int width_ = 0, height_ = 0;

    // Programs
    GLuint noteProg_ = 0;
    GLuint rectProg_ = 0;
    GLuint gradProg_ = 0;
    GLuint skewProg_ = 0;
    GLuint textProg_ = 0;
    GLuint bgProg_   = 0;   // stretched background-image quad

    // VBOs (one per instanced pipeline + the shared unit quad)
    GLuint instVbo_     = 0;
    GLuint rectInstVbo_ = 0;
    GLuint gradInstVbo_ = 0;
    GLuint skewInstVbo_ = 0;
    GLuint glyphVbo_    = 0;
    GLuint quadVbo_  = 0;
    GLuint fontTex_  = 0;
    GLuint bgTex_    = 0;      // user background image (0 = none)
    bool   hasBgImage_ = false;

    // Instancing layer (resolved in initEGL). When useNativeInstancing_ is false
    // the emulated path expands instances to triangles using cornerVbo_.
    bool   useNativeInstancing_ = false;
    // Platform-neutral function-pointer types (the GLES2 PFNGL*EXTPROC typedefs
    // are Android-only; iOS binds the core ES3 entry points directly). The
    // signatures match both the ES3 core functions and the ES2 EXT/ANGLE/NV ext.
    typedef void (*DrawArraysInstancedFn)(GLenum, GLint, GLsizei, GLsizei);
    typedef void (*VertexAttribDivisorFn)(GLuint, GLuint);
    DrawArraysInstancedFn  pDrawArraysInstanced_ = nullptr;
    VertexAttribDivisorFn  pVertexAttribDivisor_ = nullptr;
    GLuint cornerVbo_      = 0;   // emulated path: 6 corners/instance, static
    int    cornerCapacity_ = 0;   // # instances cornerVbo_ is currently sized for
    std::vector<uint8_t> expandScratch_;  // emulated path: CPU expansion buffer

    // Keyboard layout
    float keyX_[128], keyW_[128];
    bool  keyBlack_[128];
    int   startNote_ = 21, endNote_ = 108;

    // Per-frame scratch buffers
    std::vector<NoteInstanceES2> notesScratch_;
    std::vector<RectInstance>    rects_;
    std::vector<GradInstance>    grads_;
    std::vector<SkewInstance>    skews_;
    std::vector<GlyphInstance>   glyphs_;
    float kbFrac_ = 0.25f;    // computed each frame: PFA aspect-ratio formula (fWhiteCX/KeyRatio), capped at height*0.25
    float glyphScale_ = 1.0f;
    // Background color (BGR like PFA). Default = PFA's m_csBackground = 0x00464646.
    uint32_t bgColor_ = 0x00464646u;
};

}  // namespace apfa
