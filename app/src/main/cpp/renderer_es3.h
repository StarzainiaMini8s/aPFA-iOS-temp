// renderer_es3.h — OpenGL ES 3.0 note field + piano keyboard (PFA-faithful) + bitmap HUD.
// The default renderer on Android (VAOs, GLSL ES 3.00, native instancing). Android-only:
// it is NOT part of the iOS Xcode target, which uses RendererES2 (renderer.cpp).
//
// Linkage note: to keep the merged libapfa.so loadable on ES2-only GPUs (so an
// ES2-only device can still launch and reach the "Legacy Renderer" toggle), the
// few ES3-only entry points (VAOs + instancing) are resolved at runtime via
// eglGetProcAddress instead of being statically linked against libGLESv3. The
// CMake link line carries only GLESv2 + EGL.
#pragma once

#include "renderer.h"

#include <cstdint>
#include <vector>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

namespace apfa {

class RendererES3 : public IRenderer {
public:
    bool initEGL(void* window) override;
    void resize(int w, int h) override;
    void setSwapInterval(bool vsync) override;
    void setNoteRange(int startNote, int endNote) override;
    void setBgColor(uint32_t bgrColor) override { bgColor_ = bgrColor; }
    void uploadBgImage(const uint8_t* rgba, int w, int h) override;
    void destroyEGL() override;
    bool valid() const override { return context_ != EGL_NO_CONTEXT; }

    void render(float clockSec, float totalSec, float fps,
                float windowSec,
                const std::vector<NoteInstance>& notes,
                const uint32_t keyColor[128]) override;

private:
    bool buildPrograms();
    void buildFontTexture();
    void layoutKeyboard(int startNote, int endNote);

    // Resolve the ES3-only entry points (VAOs + instancing) via eglGetProcAddress.
    // Returns false if any are missing (i.e. no real ES3 context) so init aborts
    // cleanly instead of crashing on a null call.
    bool resolveES3Entry();

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

    EGLDisplay     display_ = EGL_NO_DISPLAY;
    EGLSurface     surface_ = EGL_NO_SURFACE;
    EGLContext     context_ = EGL_NO_CONTEXT;
    void*          window_  = nullptr;     // ANativeWindow* (opaque on this side)
    int width_ = 0, height_ = 0;

    // Dynamically-resolved ES3 entry points (see resolveES3Entry / linkage note).
    typedef void (*GenVertexArraysFn)(GLsizei, GLuint*);
    typedef void (*BindVertexArrayFn)(GLuint);
    typedef void (*DrawArraysInstancedFn)(GLenum, GLint, GLsizei, GLsizei);
    typedef void (*VertexAttribDivisorFn)(GLuint, GLuint);
    GenVertexArraysFn     pGenVertexArrays_     = nullptr;
    BindVertexArrayFn     pBindVertexArray_     = nullptr;
    DrawArraysInstancedFn pDrawArraysInstanced_ = nullptr;
    VertexAttribDivisorFn pVertexAttribDivisor_ = nullptr;

    // Programs
    GLuint noteProg_ = 0;
    GLuint rectProg_ = 0;
    GLuint gradProg_ = 0;
    GLuint skewProg_ = 0;
    GLuint textProg_ = 0;
    GLuint bgProg_   = 0;   // stretched background-image quad

    // VAOs / VBOs
    GLuint noteVao_  = 0, instVbo_     = 0;
    GLuint rectVao_  = 0, rectInstVbo_ = 0;
    GLuint gradVao_  = 0, gradInstVbo_ = 0;
    GLuint skewVao_  = 0, skewInstVbo_ = 0;
    GLuint textVao_  = 0, glyphVbo_    = 0;
    GLuint bgVao_    = 0;
    GLuint quadVbo_  = 0;
    GLuint fontTex_  = 0;
    GLuint bgTex_    = 0;      // user background image (0 = none)
    bool   hasBgImage_ = false;

    // Keyboard layout
    float keyX_[128], keyW_[128];
    bool  keyBlack_[128];
    int   startNote_ = 21, endNote_ = 108;

    // Per-frame scratch buffers
    std::vector<NoteInstance>  notesScratch_;
    std::vector<RectInstance>  rects_;
    std::vector<GradInstance>  grads_;
    std::vector<SkewInstance>  skews_;
    std::vector<GlyphInstance> glyphs_;
    float kbFrac_ = 0.25f;    // computed each frame: PFA aspect-ratio formula (fWhiteCX/KeyRatio), capped at height*0.25
    float glyphScale_ = 1.0f;
    // Background color (BGR like PFA). Default = PFA's m_csBackground = 0x00464646.
    uint32_t bgColor_ = 0x00464646u;
};

}  // namespace apfa
