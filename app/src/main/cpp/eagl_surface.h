// eagl_surface.h — iOS EAGL context/surface shim interface.
//
// Pure C++ declarations (no Objective-C), so renderer.cpp can call these without
// being compiled as Objective-C++. The implementation lives in ios/eagl_surface.mm
// and is the iOS analog of the EGL display/surface/context that renderer.cpp uses
// on Android. All functions run on the engine thread, which owns the GL context.
#pragma once
#if defined(__APPLE__)

namespace apfa {

// Create an EAGLContext (prefers ES3 -> native instancing, falls back to ES2),
// a colour renderbuffer bound to `caeaglLayer` (a CAEAGLLayer*), and an FBO.
// Makes the context current on the calling thread. Returns an opaque handle, or
// nullptr on failure. Writes the renderbuffer pixel size into outW/outH and
// whether an ES3 context was obtained into outHaveES3.
void* eaglCreate(void* caeaglLayer, int* outW, int* outH, bool* outHaveES3);

// Make this surface's context current on the calling thread (idempotent).
void  eaglMakeCurrent(void* surf);

// Bind the layer-backed FBO as the draw target (no default framebuffer 0 on iOS).
void  eaglBindDrawable(void* surf);

// Re-allocate the renderbuffer from the layer if its drawable size changed, then
// write the current pixel size into outW/outH. Cheap when size is unchanged.
void  eaglEnsureSize(void* surf, int* outW, int* outH);

// End of frame: glFinish() (so GPU cost enters the one-frame-delayed clock the way
// eglSwapBuffers' vblank stall did on Android) then presentRenderbuffer.
void  eaglPresent(void* surf);

// Tear down FBO, renderbuffer, and context. Releases the handle.
void  eaglDestroy(void* surf);

}  // namespace apfa

#endif  // __APPLE__
