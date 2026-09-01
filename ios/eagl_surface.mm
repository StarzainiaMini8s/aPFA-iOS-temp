// eagl_surface.mm — iOS EAGL context/surface, the analog of the EGL display /
// surface / context block in renderer.cpp::initEGL. Declared in
// app/src/main/cpp/eagl_surface.h; see that header for the contract.
//
// EVERY function here runs on the ENGINE thread, which owns the GL context —
// the same thread that runs the clock, dispatch, buildVisible and the draw
// calls, exactly as PFA's GameThread does. The only thing the main thread is
// allowed to touch is the CAEAGLLayer's own CALayer properties (bounds,
// contentsScale, drawableProperties, opaque), and it must have finished doing
// so before Engine::start() hands the layer over — CALayer is not thread-safe
// and we deliberately never read layer geometry from here. The renderbuffer's
// pixel size comes from glGetRenderbufferParameteriv instead, which is the
// authoritative number anyway (it is what the drawable actually allocated).
//
// No depth or stencil renderbuffer is created. renderer.cpp does
// glDisable(GL_DEPTH_TEST) at init and never turns it back on, so a depth
// buffer would be pure waste — and on the floor device (iPhone 5S, ~1 GB RAM)
// a full-screen depth buffer is memory that the note tables want more.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <OpenGLES/ES3/gl.h>
#import <OpenGLES/ES3/glext.h>

#include "eagl_surface.h"
#include "platform.h"   // LOGI / LOGE

#if !__has_feature(objc_arc)
#error "eagl_surface.mm must be compiled with ARC (-fobjc-arc)."
#endif

namespace {

// ARC manages the two Objective-C members because this is Objective-C++ and the
// struct is created with new / destroyed with delete (never malloc'd, which ARC
// could not hook).
struct EaglSurface {
    EAGLContext* ctx     = nil;
    CAEAGLLayer* layer   = nil;
    GLuint       fbo     = 0;
    GLuint       colorRb = 0;
    int          w       = 0;
    int          h       = 0;
};

// Re-attach the colour renderbuffer to the layer and read back the size the
// drawable actually allocated. Context must be current, colorRb bound.
bool allocFromLayer(EaglSurface* s) {
    if (![s->ctx renderbufferStorage:GL_RENDERBUFFER fromDrawable:s->layer]) {
        LOGE("eagl: renderbufferStorage:fromDrawable: failed");
        return false;
    }
    GLint w = 0, h = 0;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH,  &w);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
    if (w <= 0 || h <= 0) {
        LOGE("eagl: drawable has zero size (%d x %d)", w, h);
        return false;
    }
    s->w = w;
    s->h = h;
    return true;
}

}  // namespace

namespace apfa {

void* eaglCreate(void* caeaglLayer, int* outW, int* outH, bool* outHaveES3) {
    if (outW)       *outW = 0;
    if (outH)       *outH = 0;
    if (outHaveES3) *outHaveES3 = false;
    if (!caeaglLayer) { LOGE("eaglCreate: null layer"); return nullptr; }

    CAEAGLLayer* layer = (__bridge CAEAGLLayer*)caeaglLayer;

    // Prefer ES3 (native instancing — resolveInstancing binds glDrawArrays-
    // Instanced / glVertexAttribDivisor directly), fall back to ES2 so the
    // emulated-instancing path still runs. Mirrors the EGL ctx3/ctx2 attempt in
    // renderer.cpp. Every A7 and later gives ES3, so the fallback is for the
    // simulator and for completeness.
    EAGLContext* ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    bool haveES3 = (ctx != nil);
    if (!ctx) ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!ctx) { LOGE("eaglCreate: could not create an EAGL context"); return nullptr; }

    if (![EAGLContext setCurrentContext:ctx]) {
        LOGE("eaglCreate: setCurrentContext failed");
        return nullptr;
    }

    EaglSurface* s = new EaglSurface();
    s->ctx   = ctx;
    s->layer = layer;

    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glGenRenderbuffers(1, &s->colorRb);
    glBindRenderbuffer(GL_RENDERBUFFER, s->colorRb);

    if (!allocFromLayer(s)) { eaglDestroy(s); return nullptr; }

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, s->colorRb);

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("eaglCreate: framebuffer incomplete (0x%04x)", (unsigned)st);
        eaglDestroy(s);
        return nullptr;
    }

    if (outW)       *outW = s->w;
    if (outH)       *outH = s->h;
    if (outHaveES3) *outHaveES3 = haveES3;
    LOGI("eagl: %s context, drawable %d x %d", haveES3 ? "ES3" : "ES2", s->w, s->h);
    return s;
}

void eaglMakeCurrent(void* surf) {
    EaglSurface* s = static_cast<EaglSurface*>(surf);
    if (!s) return;
    // Idempotent: EAGL no-ops when the context is already current on this thread.
    [EAGLContext setCurrentContext:s->ctx];
}

void eaglBindDrawable(void* surf) {
    EaglSurface* s = static_cast<EaglSurface*>(surf);
    if (!s) return;
    // There is no screen-backed framebuffer 0 on iOS — every frame must bind the
    // layer-backed FBO before drawing, where Android just renders into the
    // window surface's default framebuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glViewport(0, 0, s->w, s->h);
}

void eaglEnsureSize(void* surf, int* outW, int* outH) {
    EaglSurface* s = static_cast<EaglSurface*>(surf);
    if (!s) return;
    // renderer.cpp only calls this when the size it was told about differs from
    // the size it is holding (startup / rotation), so the re-allocation here is
    // never on the per-frame path.
    glBindRenderbuffer(GL_RENDERBUFFER, s->colorRb);
    if (allocFromLayer(s)) {
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, s->colorRb);
        LOGI("eagl: drawable resized to %d x %d", s->w, s->h);
    }
    if (outW) *outW = s->w;
    if (outH) *outH = s->h;
}

void eaglPresent(void* surf) {
    EaglSurface* s = static_cast<EaglSurface*>(surf);
    if (!s) return;
    glBindRenderbuffer(GL_RENDERBUFFER, s->colorRb);
    // THE timing-critical line of the whole port. On Android the frame's GPU cost
    // enters the one-frame-delayed clock because eglSwapBuffers blocks on vblank
    // — that stall is measured inside the frame, the same way D3D Present()
    // enters PFA's clock. presentRenderbuffer alone does NOT block that way: it
    // queues and returns, so a GPU-bound frame would look free to the clock and
    // aPFA would report a frame rate the hardware is not actually achieving.
    // glFinish() restores the fence: it returns only once the GPU has finished
    // the frame, so GPU cost stretches frame time here exactly as it does on
    // Android. Do not "optimise" this away — a slow device is SUPPOSED to slow
    // down, and this is the line that makes it.
    glFinish();
    [s->ctx presentRenderbuffer:GL_RENDERBUFFER];
}

void eaglDestroy(void* surf) {
    EaglSurface* s = static_cast<EaglSurface*>(surf);
    if (!s) return;
    if (s->ctx) {
        [EAGLContext setCurrentContext:s->ctx];
        if (s->colorRb) glDeleteRenderbuffers(1, &s->colorRb);
        if (s->fbo)     glDeleteFramebuffers(1, &s->fbo);
        [EAGLContext setCurrentContext:nil];
    }
    s->ctx   = nil;   // ARC releases both here...
    s->layer = nil;
    delete s;         // ...and again for anything left, on destruction.
}

}  // namespace apfa
