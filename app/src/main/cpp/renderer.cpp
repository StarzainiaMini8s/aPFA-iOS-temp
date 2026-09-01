// renderer.cpp — RendererES2 impl. See renderer_es2.h / renderer.h.
#include "renderer_es2.h"
#include "font_data.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "platform.h"
#if defined(__APPLE__)
#include "eagl_surface.h"   // EAGL context/surface shim (eagl_surface.mm)
#else
#include <android/native_window.h>
#include "renderer_es3.h"   // RendererES3 — referenced by createRenderer() below (Android only)
#endif

namespace apfa {

// kbFrac_ is computed dynamically in render() using PFA's aspect-ratio formula:
// ideal keyboard height = (fWhiteCX / KeyRatio), capped at height * KBPercent (0.25).
// This matches PFA's GameState.cpp logic and prevents vertical stretching.

// ---- shaders ----------------------------------------------------------------

// NB: the per-key x/width comes in as a per-instance vec2 attribute (aKeyXW),
// NOT a dynamically indexed uniform array. GLSL ES 1.00 does not guarantee
// dynamic indexing of uniform arrays in the vertex shader (fails on Mali-400
// class ES2 GPUs), and it has no integer overload of clamp() — both of which the
// old `uKey[clamp(int(aKey+0.5),0,127)]` form relied on. Feeding x/width as an
// attribute sidesteps both. The renderer fills aKeyXW from keyX_/keyW_.
static const char* kNoteVS = R"(#version 100
attribute vec2 aQuad;
attribute float aStartSec;
attribute float aDurSec;
attribute vec2 aKeyXW;       // x = normalised key left edge, y = normalised key width
attribute vec4 aColorPrimary;
attribute vec4 aColorDark;
attribute vec4 aColorVeryDark;
attribute float aIsSharp;
uniform float uClockSec;
uniform float uWindowSec;
uniform float uKbFrac;
uniform vec2  uViewportPx;
varying vec4 vColorPrimary;
varying vec4 vColorDark;
varying vec4 vColorVeryDark;
varying vec2 vUV;
varying vec2 vSizePx;
void main() {
    float ab = uKbFrac;
    float sf = (aStartSec - uClockSec) / uWindowSec;
    float ef = (aStartSec + aDurSec - uClockSec) / uWindowSec;
    float yStart = ab + (1.0 - ab) * sf;
    float yEnd   = ab + (1.0 - ab) * ef;
    float x = aKeyXW.x + aQuad.x * aKeyXW.y;
    float y = mix(yStart, yEnd, aQuad.y);
    vUV = aQuad;
    vColorPrimary  = aColorPrimary;
    vColorDark     = aColorDark;
    vColorVeryDark = aColorVeryDark;
    vSizePx = vec2(aKeyXW.y * uViewportPx.x, abs(yEnd - yStart) * uViewportPx.y);
    gl_Position = vec4(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kNoteFS = R"(#version 100
precision mediump float;
varying vec4 vColorPrimary;
varying vec4 vColorDark;
varying vec4 vColorVeryDark;
varying vec2 vUV;
varying vec2 vSizePx;
uniform float uWhiteKeyPx;
void main() {
    vec2 px = vUV * vSizePx;
    float b = clamp(floor(uWhiteKeyPx * 0.075 + 0.5), 1.0, 3.0);
    bool border = px.x < b || px.y < b ||
                  (vSizePx.x - px.x) < b || (vSizePx.y - px.y) < b;
    if (border) {
        gl_FragColor = vec4(vColorVeryDark.rgb, 1.0);
    } else {
        vec2 inner = (px - vec2(b)) / (vSizePx - vec2(b * 2.0));
        float t = inner.x;
        vec3 c = mix(vColorPrimary.rgb, vColorDark.rgb, t);
        gl_FragColor = vec4(c, 1.0);
    }
}
)";

// Rect shader — instanced axis-aligned quads in NDC [0..1] normalised coords.
// Each instance: x,y,w,h in normalised [0..1] and a solid RGBA color.
static const char* kRectVS = R"(#version 100
attribute vec2 aQuad;
attribute vec4 aRect;
attribute vec4 aColor;
varying vec4 vColor;
void main() {
    float x = aRect.x + aQuad.x * aRect.z;
    float y = aRect.y + aQuad.y * aRect.w;
    vColor = aColor;
    gl_Position = vec4(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kRectFS = R"(#version 100
precision mediump float;
varying vec4 vColor;
void main() { gl_FragColor = vec4(vColor.rgb, vColor.a); }
)";

// Gradient rect shader — 4 per-corner colours (TL, TR, BR, BL) in normalised coords.
// PFA DrawRect(x,y,cx,cy, c1,c2,c3,c4): c1=TL, c2=TR, c3=BR, c4=BL
// We mirror y so c1 is screen-top-left = GL bottom-left in y-flipped coords.
// Layout: x,y,w,h, c1,c2,c3,c4 (each 4 bytes = 4*4 + 4*4 = 32 bytes)
static const char* kGradVS = R"(#version 100
attribute vec2 aQuad;        // 0,0 .. 1,1
attribute vec4 aRect;        // x,y,w,h  normalised [0..1]
attribute vec4 aCTL;         // top-left color
attribute vec4 aCTR;         // top-right color
attribute vec4 aCBR;         // bottom-right color
attribute vec4 aCBL;         // bottom-left color
varying vec4 vColor;
void main() {
    // aQuad: x=0..1 left-to-right, y=0..1 bottom-to-top (GL default)
    // Screen top = y=1 in GL. So "top" colors correspond to aQuad.y==1.
    vec4 top    = mix(aCTL, aCTR, aQuad.x);
    vec4 bottom = mix(aCBL, aCBR, aQuad.x);
    vColor = mix(bottom, top, aQuad.y);
    float x = aRect.x + aQuad.x * aRect.z;
    float y = aRect.y + aQuad.y * aRect.w;
    gl_Position = vec4(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kGradFS = R"(#version 100
precision mediump float;
varying vec4 vColor;
void main() { gl_FragColor = vColor; }
)";

// Skew shader — arbitrary quad (4 NDC-space corner vertices) for 3-D black keys.
// Each instance: 4×vec2 corners (TL,TR,BR,BL in screen space normalised),
// and 4 RGBA corner colors.
// Total: 8 floats corners + 4*4 bytes colors = 32+16 = 48 bytes/instance.
static const char* kSkewVS = R"(#version 100
attribute vec2 aQuad;      // 0,0 .. 1,1
// per-instance corners: TL, TR, BR, BL (normalised 0..1 screen space)
attribute vec2 aCTL;
attribute vec2 aCTR;
attribute vec2 aCBR;
attribute vec2 aCBL;
// per-instance corner colours: TL, TR, BR, BL
attribute vec4 aColTL;
attribute vec4 aColTR;
attribute vec4 aColBR;
attribute vec4 aColBL;
varying vec4 vColor;
void main() {
    // Bilinear interpolation of position using GL quad convention:
    // aQuad.x=0 left, aQuad.x=1 right, aQuad.y=0 bottom, aQuad.y=1 top.
    // Our corner names: TL=top-left (y=1,x=0), TR=top-right (y=1,x=1),
    //                   BR=bottom-right(y=0,x=1), BL=bottom-left(y=0,x=0)
    vec2 top    = mix(aCTL, aCTR, aQuad.x);
    vec2 bottom = mix(aCBL, aCBR, aQuad.x);
    vec2 pos    = mix(bottom, top, aQuad.y);
    vec4 ctop    = mix(aColTL, aColTR, aQuad.x);
    vec4 cbottom = mix(aColBL, aColBR, aQuad.x);
    vColor = mix(cbottom, ctop, aQuad.y);
    gl_Position = vec4(pos.x * 2.0 - 1.0, pos.y * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kSkewFS = R"(#version 100
precision mediump float;
varying vec4 vColor;
void main() { gl_FragColor = vColor; }
)";

// Text shader
static const char* kTextVS = R"(#version 100
attribute vec2 aQuad;
attribute vec2 aPos;
attribute float aU;
attribute vec4 aColor;
uniform vec2 uViewport;
uniform vec2 uGlyphPx;
uniform float uAtlasUStep;
varying vec2 vTex;
varying vec4 vColor;
void main() {
    vec2 px = aPos + vec2(aQuad.x, 1.0 - aQuad.y) * uGlyphPx;
    vec2 ndc = px / uViewport * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vTex = vec2(aU + aQuad.x * uAtlasUStep, aQuad.y);
    vColor = aColor;
}
)";

static const char* kTextFS = R"(#version 100
precision mediump float;
varying vec2 vTex;
varying vec4 vColor;
uniform sampler2D uAtlas;
void main() {
    float a = texture2D(uAtlas, vTex).r;
    if (a < 0.5) discard;
    gl_FragColor = vec4(vColor.rgb, 1.0);
}
)";

// Background-image shader — one stretched quad filling the note field.
// aQuad (0..1) maps x→[-1,1] and y→[uYBottom,1] in NDC; texture is sampled with
// v flipped so image row 0 sits at the top of the screen. Aspect is NOT preserved
// (deliberate "stretch in MS Paint" behaviour).
static const char* kBgVS = R"(#version 100
attribute vec2 aQuad;
uniform float uYBottom;
varying vec2 vTex;
void main() {
    float x = aQuad.x * 2.0 - 1.0;
    float y = mix(uYBottom, 1.0, aQuad.y);
    gl_Position = vec4(x, y, 0.0, 1.0);
    vTex = vec2(aQuad.x, 1.0 - aQuad.y);
}
)";

static const char* kBgFS = R"(#version 100
precision mediump float;
varying vec2 vTex;
uniform sampler2D uTex;
void main() { gl_FragColor = vec4(texture2D(uTex, vTex).rgb, 1.0); }
)";

// ---- GL helpers -------------------------------------------------------------

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader compile: %s", log); glDeleteShader(s); return 0;
    }
    return s;
}

// GLSL ES 1.00 has no layout(location=) qualifier, so attribute locations must be
// bound by name before linking. Each program supplies its attribute name→location map.
struct AttrBind { GLuint loc; const char* name; };

static GLuint linkProgram(const char* vs, const char* fs,
                          const AttrBind* binds, int nbinds) {
    GLuint v = compileShader(GL_VERTEX_SHADER,   vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    for (int i = 0; i < nbinds; i++) glBindAttribLocation(p, binds[i].loc, binds[i].name);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("program link: %s", log); glDeleteProgram(p); return 0;
    }
    return p;
}

// ---- PFA color helpers ------------------------------------------------------

// Pack float RGB (0..1) into RGBA8 (R in LSB, like PFA's ARGB with A=0).
// GL_UNSIGNED_BYTE with vec4 reads memory bytes as (x=byte0, y=byte1, z=byte2, w=byte3).
// On little-endian, uint32_t byte0 = LSB. We want byte0=R, byte1=G, byte2=B, byte3=A.
// So the correct packing is: (A<<24)|(B<<16)|(G<<8)|R.
static uint32_t packRGB(float r, float g, float b) {
    int R = (int)(r * 255.0f + 0.5f); if (R > 255) R = 255;
    int G = (int)(g * 255.0f + 0.5f); if (G > 255) G = 255;
    int B = (int)(b * 255.0f + 0.5f); if (B > 255) B = 255;
    return (0xFF << 24) | (B << 16) | (G << 8) | R;   // bytes: R,G,B,A → GL reads vec4(R,G,B,A)
}

// PFA SetColor: iColor is BGR (R in bit0) per Windows COLORREF.
// dDark multiplies HSV-Value for dark variant, dVeryDark for veryDark.
struct KbColor {
    uint32_t primary, dark, veryDark;
};

static void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    if (s <= 0.0f) { r = g = b = v; return; }
    float hh = h * 6.0f;
    int   i  = (int)hh;
    float ff = hh - i;
    float p  = v * (1.0f - s);
    float q  = v * (1.0f - s * ff);
    float t  = v * (1.0f - s * (1.0f - ff));
    switch (i % 6) {
        case 0: r=v; g=t; b=p; break;
        case 1: r=q; g=v; b=p; break;
        case 2: r=p; g=v; b=t; break;
        case 3: r=p; g=q; b=v; break;
        case 4: r=t; g=p; b=v; break;
        default:r=v; g=p; b=q; break;
    }
}

static KbColor makeKbColor(unsigned iColor, float dDark, float dVeryDark) {
    // iColor: BGR like PFA (R=bit0, G=bit8, B=bit16, A=bit24)
    float R = ((iColor >>  0) & 0xFF) / 255.0f;
    float G = ((iColor >>  8) & 0xFF) / 255.0f;
    float B = ((iColor >> 16) & 0xFF) / 255.0f;
    float vmax = R>G?(R>B?R:B):(G>B?G:B);
    float vmin = R<G?(R<B?R:B):(G<B?G:B);
    float v = vmax;
    float s = (vmax > 0.0f ? (vmax - vmin) / vmax : 0.0f);
    float h = 0.0f;
    if (vmax != vmin) {
        float d = vmax - vmin;
        if      (vmax == R) h = (G - B) / d + (G < B ? 6.0f : 0.0f);
        else if (vmax == G) h = (B - R) / d + 2.0f;
        else                h = (R - G) / d + 4.0f;
        h /= 6.0f;
    }
    float dr, dg, db, vdr, vdg, vdb;
    float vd = v * dDark;     if (vd > 1.0f) vd = 1.0f;
    float vvd = v * dVeryDark; if (vvd > 1.0f) vvd = 1.0f;
    hsvToRgb(h, s, vd,  dr, dg, db);
    hsvToRgb(h, s, vvd, vdr, vdg, vdb);
    KbColor kc;
    kc.primary  = packRGB(R, G, B);
    kc.dark     = packRGB(dr, dg, db);
    kc.veryDark = packRGB(vdr, vdg, vdb);
    return kc;
}

// Make a KbColor from a pressed note primary colour (engine RGBA8: R=byte0=LSB)
static KbColor makeActiveColor(uint32_t primaryRGBA) {
    // Engine packHSV format: R=byte0(LSB), G=byte1, B=byte2, A=byte3 — same as our packRGB
    float R = ((primaryRGBA >>  0) & 0xFF) / 255.0f;
    float G = ((primaryRGBA >>  8) & 0xFF) / 255.0f;
    float B = ((primaryRGBA >> 16) & 0xFF) / 255.0f;
    float vmax = R>G?(R>B?R:B):(G>B?G:B);
    float vmin = R<G?(R<B?R:B):(G<B?G:B);
    float v = vmax;
    float s = (vmax > 0.0f ? (vmax - vmin) / vmax : 0.0f);
    float h = 0.0f;
    if (vmax != vmin) {
        float d = vmax - vmin;
        if      (vmax == R) h = (G - B) / d + (G < B ? 6.0f : 0.0f);
        else if (vmax == G) h = (B - R) / d + 2.0f;
        else                h = (R - G) / d + 4.0f;
        h /= 6.0f;
    }
    float dr, dg, db, vdr, vdg, vdb;
    hsvToRgb(h, s, v * 0.6f, dr, dg, db);
    hsvToRgb(h, s, v * 0.2f, vdr, vdg, vdb);
    KbColor kc;
    kc.primary  = primaryRGBA;
    kc.dark     = packRGB(dr, dg, db);
    kc.veryDark = packRGB(vdr, vdg, vdb);
    return kc;
}

// ---- keyboard layout --------------------------------------------------------

static bool pfaIsSharp(int note) {
    int pc = note % 12;
    return pc==1||pc==3||pc==6||pc==8||pc==10;
}
static int pfaWhiteCount(int a, int b) {
    int n = 0;
    for (int i = a; i < b && i < 128; i++) if (!pfaIsSharp(i)) n++;
    return n;
}
enum PfaNote { A=9,AS_=10,B_=11,C_=0,CS_=1,D_=2,DS_=3,E_=4,F_=5,FS_=6,G_=7,GS_=8 };
static PfaNote pfaNoteVal(int note) { return static_cast<PfaNote>(note % 12); }

void RendererES2::layoutKeyboard(int startNote, int endNote) {
    startNote_ = startNote;
    endNote_   = endNote;
    const float SharpRatio = 0.65f;

    int allWhite = pfaWhiteCount(startNote, endNote + 1);
    float fBuffer = (pfaIsSharp(startNote) ? SharpRatio / 2.0f : 0.0f) +
                    (pfaIsSharp(endNote)   ? SharpRatio / 2.0f : 0.0f);

    float ww = 1.0f / (allWhite + fBuffer);
    float bw = ww * SharpRatio;

    for (int k = 0; k < 128; k++) {
        keyBlack_[k] = pfaIsSharp(k);
        float cx = pfaIsSharp(k) ? bw : ww;
        keyW_[k] = cx;

        int iWhiteKeys = pfaWhiteCount(startNote, k);
        float fStartX = (pfaIsSharp(startNote) ? 1.0f : 0.0f) * SharpRatio / 2.0f
                      - (pfaIsSharp(k)         ? 1.0f : 0.0f) * SharpRatio / 2.0f;

        if (pfaIsSharp(k)) {
            PfaNote eNote = pfaNoteVal(k);
            if      (eNote == CS_ || eNote == FS_) fStartX -= SharpRatio / 5.0f;
            else if (eNote == AS_ || eNote == DS_) fStartX += SharpRatio / 5.0f;
        }

        keyX_[k] = ww * (iWhiteKeys + fStartX);
    }
}

// ---- font texture -----------------------------------------------------------

void RendererES2::buildFontTexture() {
    glGenTextures(1, &fontTex_);
    glBindTexture(GL_TEXTURE_2D, fontTex_);
    // Single-channel atlas. GL_LUMINANCE works on both ES2 and ES3 (unlike the
    // ES3-only GL_R8/GL_RED); the shader's .r swizzle reads the replicated value.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 kAtlasW, kAtlasH, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, kFontAtlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOGI("Font atlas uploaded: %dx%d", kAtlasW, kAtlasH);
}

// ---- program init -----------------------------------------------------------

bool RendererES2::buildPrograms() {
    static const AttrBind noteB[] = {
        {0,"aQuad"},{1,"aStartSec"},{2,"aDurSec"},{3,"aKeyXW"},
        {4,"aColorPrimary"},{5,"aColorDark"},{6,"aColorVeryDark"},{7,"aIsSharp"} };
    static const AttrBind rectB[] = { {0,"aQuad"},{1,"aRect"},{2,"aColor"} };
    static const AttrBind gradB[] = {
        {0,"aQuad"},{1,"aRect"},{2,"aCTL"},{3,"aCTR"},{4,"aCBR"},{5,"aCBL"} };
    static const AttrBind skewB[] = {
        {0,"aQuad"},{1,"aCTL"},{2,"aCTR"},{3,"aCBR"},{4,"aCBL"},
        {5,"aColTL"},{6,"aColTR"},{7,"aColBR"},{8,"aColBL"} };
    static const AttrBind textB[] = { {0,"aQuad"},{1,"aPos"},{2,"aU"},{3,"aColor"} };
    static const AttrBind bgB[]   = { {0,"aQuad"} };

    noteProg_ = linkProgram(kNoteVS, kNoteFS, noteB, 8);
    rectProg_ = linkProgram(kRectVS, kRectFS, rectB, 3);
    gradProg_ = linkProgram(kGradVS, kGradFS, gradB, 6);
    skewProg_ = linkProgram(kSkewVS, kSkewFS, skewB, 9);
    textProg_ = linkProgram(kTextVS, kTextFS, textB, 4);
    bgProg_   = linkProgram(kBgVS,   kBgFS,   bgB,   1);
    return noteProg_ && rectProg_ && gradProg_ && skewProg_ && textProg_ && bgProg_;
}

// ---- EGL init ---------------------------------------------------------------

bool RendererES2::initEGL(void* window) {
    window_ = window;
    bool haveES3 = false;
#if defined(__APPLE__)
    // iOS: the EAGL context + a renderbuffer bound to the CAEAGLLayer + an FBO
    // are created in eagl_surface.mm. It prefers an ES3 context (native
    // instancing) and falls back to ES2, mirroring the EGL path below.
    glSurface_ = eaglCreate(window_, &width_, &height_, &haveES3);
    if (!glSurface_) { LOGE("eaglCreate failed"); return false; }
#else
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
        LOGE("eglInitialize failed"); return false;
    }
    const EGLint cfgAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config; EGLint numCfg = 0;
    if (!eglChooseConfig(display_, cfgAttribs, &config, 1, &numCfg) || numCfg < 1) {
        LOGE("eglChooseConfig failed"); return false;
    }
    surface_ = eglCreateWindowSurface(display_, config,
                                      (EGLNativeWindowType)window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); return false; }
    // Prefer an ES 3.0 context (native instancing, fastest path); fall back to a
    // baseline ES 2.0 context so ES2-only GPUs still run.
    const EGLint ctx3Attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx3Attribs);
    haveES3 = (context_ != EGL_NO_CONTEXT);
    if (!haveES3) {
        const EGLint ctx2Attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx2Attribs);
    }
    if (context_ == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return false; }
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGE("eglMakeCurrent failed"); return false;
    }
    EGLint w = 0, h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH,  &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    width_ = w; height_ = h;
#endif
    resolveInstancing(haveES3);

    if (!buildPrograms()) return false;
    buildFontTexture();
    layoutKeyboard(21, 108);

    static const float quad[8] = { 0,0, 1,0, 0,1, 1,1 };
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glGenBuffers(1, &instVbo_);
    glGenBuffers(1, &rectInstVbo_);
    glGenBuffers(1, &gradInstVbo_);
    glGenBuffers(1, &skewInstVbo_);
    glGenBuffers(1, &glyphVbo_);

    // Per-pipeline vertex attribute layout is set up per draw inside drawInstanced()
    // (no VAOs — they are ES3 core / an ES2 extension, and we want a single path
    // that runs on baseline ES 2.0). Attribute descriptor tables live at the draw
    // sites; the emulated-instancing corner buffer is created lazily.

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    LOGI("Renderer ready: GL %s, %dx%d",
         reinterpret_cast<const char*>(glGetString(GL_VERSION)), width_, height_);
    return true;
}

// ---- instancing layer -------------------------------------------------------

// Resolve the instancing entry points. On an ES 3.0 context the core functions are
// used; on ES 2.0 we look for the EXT/ANGLE/NV instanced-arrays extensions. If none
// is available useNativeInstancing_ stays false and drawInstanced() emulates it.
void RendererES2::resolveInstancing(bool haveES3Context) {
    pDrawArraysInstanced_ = nullptr;
    pVertexAttribDivisor_ = nullptr;

#if defined(__APPLE__)
    // iOS: no eglGetProcAddress. With an ES3 context the core instancing entry
    // points are statically linked from the OpenGLES framework — bind directly.
    // On an ES2-only iOS context (not the case on A7+, which give ES3) we fall
    // through to the CPU-emulated path. Cast guards against GL_APIENTRY/calling
    // convention typedef differences between the framework and our fn typedefs.
    if (haveES3Context) {
        pDrawArraysInstanced_ = (DrawArraysInstancedFn)&glDrawArraysInstanced;
        pVertexAttribDivisor_ = (VertexAttribDivisorFn)&glVertexAttribDivisor;
    }
#else
    if (haveES3Context) {
        pDrawArraysInstanced_ = (DrawArraysInstancedFn)
            eglGetProcAddress("glDrawArraysInstanced");
        pVertexAttribDivisor_ = (VertexAttribDivisorFn)
            eglGetProcAddress("glVertexAttribDivisor");
    }
    if (!pDrawArraysInstanced_ || !pVertexAttribDivisor_) {
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        auto has = [ext](const char* n) { return ext && strstr(ext, n) != nullptr; };
        const char* drawName = nullptr;
        const char* divName  = nullptr;
        if (has("GL_EXT_instanced_arrays")) {
            drawName = "glDrawArraysInstancedEXT"; divName = "glVertexAttribDivisorEXT";
        } else if (has("GL_ANGLE_instanced_arrays")) {
            drawName = "glDrawArraysInstancedANGLE"; divName = "glVertexAttribDivisorANGLE";
        } else if (has("GL_NV_instanced_arrays") && has("GL_NV_draw_instanced")) {
            drawName = "glDrawArraysInstancedNV"; divName = "glVertexAttribDivisorNV";
        }
        if (drawName) {
            pDrawArraysInstanced_ = (DrawArraysInstancedFn)eglGetProcAddress(drawName);
            pVertexAttribDivisor_ = (VertexAttribDivisorFn)eglGetProcAddress(divName);
        }
    }
#endif
    useNativeInstancing_ = (pDrawArraysInstanced_ && pVertexAttribDivisor_);
    LOGI("instancing: %s (ES3 ctx=%d)",
         useNativeInstancing_ ? "native" : "emulated", (int)haveES3Context);
}

// Make sure cornerVbo_ holds the 6-corner two-triangle pattern repeated `count`
// times — used only by the emulated (no native instancing) path.
void RendererES2::ensureCorners(int count) {
    if (count <= cornerCapacity_) return;
    int cap = count < 4096 ? 4096 : count;
    // Two triangles covering the unit quad: (0,0)(1,0)(0,1) and (1,0)(0,1)(1,1).
    static const float pat[12] = { 0,0, 1,0, 0,1,  1,0, 0,1, 1,1 };
    std::vector<float> buf;
    buf.reserve((size_t)cap * 12);
    for (int i = 0; i < cap; i++)
        for (int j = 0; j < 12; j++) buf.push_back(pat[j]);
    if (cornerVbo_ == 0) glGenBuffers(1, &cornerVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, cornerVbo_);
    glBufferData(GL_ARRAY_BUFFER, buf.size() * sizeof(float), buf.data(), GL_STATIC_DRAW);
    cornerCapacity_ = cap;
}

// Draw `count` unit-quad instances through `prog`. The caller has already set up
// `prog`'s uniforms (and any texture binds). Per-instance data is `count*stride`
// bytes at `data`, uploaded into `instVbo`. Location 0 is the quad corner.
void RendererES2::drawInstanced(GLuint prog, const void* data, int count,
                             GLsizei stride, const VAttr* attrs, int nattrs,
                             GLuint instVbo) {
    if (count <= 0) return;
    glUseProgram(prog);
    // Clear any attribute arrays left enabled by a previous (wider) pipeline.
    for (GLuint i = 0; i <= 8; i++) glDisableVertexAttribArray(i);

    if (useNativeInstancing_) {
        // Location 0: shared unit quad, advances per vertex (divisor 0).
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
        pVertexAttribDivisor_(0, 0);
        // Per-instance attributes from the (non-expanded) instance buffer, divisor 1.
        glBindBuffer(GL_ARRAY_BUFFER, instVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * stride, data, GL_DYNAMIC_DRAW);
        for (int i = 0; i < nattrs; i++) {
            const VAttr& a = attrs[i];
            glEnableVertexAttribArray(a.loc);
            glVertexAttribPointer(a.loc, a.size, a.type, a.norm, stride, (void*)(size_t)a.offset);
            pVertexAttribDivisor_(a.loc, 1);
        }
        pDrawArraysInstanced_(GL_TRIANGLE_STRIP, 0, 4, count);
    } else {
        // Emulated: expand each instance to 6 vertices (two triangles), duplicating
        // its per-instance record so every attribute advances per vertex. The unit
        // quad corner comes from cornerVbo_; everything else from the expanded buffer.
        ensureCorners(count);
        const size_t need = (size_t)count * 6 * stride;
        if (expandScratch_.size() < need) expandScratch_.resize(need);
        const uint8_t* src = static_cast<const uint8_t*>(data);
        uint8_t* dst = expandScratch_.data();
        for (int i = 0; i < count; i++) {
            const uint8_t* s = src + (size_t)i * stride;
            for (int v = 0; v < 6; v++) { memcpy(dst, s, stride); dst += stride; }
        }
        glBindBuffer(GL_ARRAY_BUFFER, instVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)need, expandScratch_.data(), GL_DYNAMIC_DRAW);
        for (int i = 0; i < nattrs; i++) {
            const VAttr& a = attrs[i];
            glEnableVertexAttribArray(a.loc);
            glVertexAttribPointer(a.loc, a.size, a.type, a.norm, stride, (void*)(size_t)a.offset);
        }
        glBindBuffer(GL_ARRAY_BUFFER, cornerVbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, count * 6);
    }
}

// Attribute descriptor tables (must match the *Instance struct layouts in renderer.h).
// Offsets match NoteInstance (renderer.h). `key` (offset 8) is read CPU-side to
// fill keyX/keyW and is NOT bound here; aKeyXW (loc 3) is the vec2 at keyX/keyW.
static const VAttr kNoteAttrs[] = {
    {1,1,GL_FLOAT,GL_FALSE,0},{2,1,GL_FLOAT,GL_FALSE,4},{3,2,GL_FLOAT,GL_FALSE,12},
    {4,4,GL_UNSIGNED_BYTE,GL_TRUE,20},{5,4,GL_UNSIGNED_BYTE,GL_TRUE,24},
    {6,4,GL_UNSIGNED_BYTE,GL_TRUE,28},{7,1,GL_FLOAT,GL_FALSE,32} };
static const VAttr kRectAttrs[] = {
    {1,4,GL_FLOAT,GL_FALSE,0},{2,4,GL_UNSIGNED_BYTE,GL_TRUE,16} };
static const VAttr kGradAttrs[] = {
    {1,4,GL_FLOAT,GL_FALSE,0},{2,4,GL_UNSIGNED_BYTE,GL_TRUE,16},
    {3,4,GL_UNSIGNED_BYTE,GL_TRUE,20},{4,4,GL_UNSIGNED_BYTE,GL_TRUE,24},
    {5,4,GL_UNSIGNED_BYTE,GL_TRUE,28} };
static const VAttr kSkewAttrs[] = {
    {1,2,GL_FLOAT,GL_FALSE,0},{2,2,GL_FLOAT,GL_FALSE,8},{3,2,GL_FLOAT,GL_FALSE,16},
    {4,2,GL_FLOAT,GL_FALSE,24},{5,4,GL_UNSIGNED_BYTE,GL_TRUE,32},
    {6,4,GL_UNSIGNED_BYTE,GL_TRUE,36},{7,4,GL_UNSIGNED_BYTE,GL_TRUE,40},
    {8,4,GL_UNSIGNED_BYTE,GL_TRUE,44} };
static const VAttr kGlyphAttrs[] = {
    {1,2,GL_FLOAT,GL_FALSE,0},{2,1,GL_FLOAT,GL_FALSE,8},{3,4,GL_UNSIGNED_BYTE,GL_TRUE,12} };

void RendererES2::resize(int w, int h) {
#if defined(__APPLE__)
    // iOS: the renderbuffer is backed by the CAEAGLLayer. Re-allocate it only
    // when the reported surface size actually changes (startup / rotation), then
    // read back the true pixel dims. Called on the engine thread (context current).
    if (glSurface_ && w > 0 && h > 0 && (w != width_ || h != height_))
        eaglEnsureSize(glSurface_, &width_, &height_);
#else
    if (w > 0 && h > 0) { width_ = w; height_ = h; }
#endif
}

void RendererES2::setNoteRange(int startNote, int endNote) {
    layoutKeyboard(startNote, endNote);
}

void RendererES2::setSwapInterval(bool vsync) {
#if defined(__APPLE__)
    // EAGL has no swap-interval control. A legit run is uncapped anyway; GPU cost
    // enters the one-frame-delayed clock via the glFinish() fence in eaglPresent.
    (void)vsync;
#else
    if (display_ != EGL_NO_DISPLAY)
        eglSwapInterval(display_, vsync ? 1 : 0);
#endif
}

void RendererES2::uploadBgImage(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) {            // drop back to the solid colour
        hasBgImage_ = false;
        return;
    }
    if (bgTex_ == 0) glGenTextures(1, &bgTex_);
    glBindTexture(GL_TEXTURE_2D, bgTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    hasBgImage_ = true;
    LOGI("Background image uploaded: %dx%d", w, h);
}

// ---- batch draw helpers (pixel-space → normalised) --------------------------

// PFA's DrawRect(x, y, cx, cy, c1, c2, c3, c4):
//   x,y = top-left in PIXEL space (y increasing DOWNWARD, as in D3D/Windows)
//   c1=TL, c2=TR, c3=BR, c4=BL
// In GL (y-up), we invert y: y_gl = (height - y - cy) [for bottom-left of rect]
// Our GradInstance stores y as GL-normalised bottom of the rect.
void RendererES2::pushGradRect(float x, float y, float cx, float cy,
                             uint32_t cTL, uint32_t cTR, uint32_t cBR, uint32_t cBL) {
    float W = (float)width_, H = (float)height_;
    GradInstance g;
    g.x  = x  / W;
    // GL y: bottom of the rect. In screen space top=y, height=cy → GL bottom = H-(y+cy)
    g.y  = (H - (y + cy)) / H;
    g.w  = cx / W;
    g.h  = cy / H;
    g.cTL = cTL; g.cTR = cTR; g.cBR = cBR; g.cBL = cBL;
    grads_.push_back(g);
}

// Flat rect helper (all 4 corners same color)
void RendererES2::pushFlatRect(float x, float y, float cx, float cy, uint32_t c) {
    pushGradRect(x, y, cx, cy, c, c, c, c);
}

// PFA's DrawSkew(x1,y1, x2,y2, x3,y3, x4,y4, c1,c2,c3,c4):
// The 4 points form an arbitrary quad. In PFA the points are given as:
//   (x1,y1)=TL, (x2,y2)=TR, (x3,y3)=BR, (x4,y4)=BL
// All in pixel space, y downward. Colors: c1=TL,c2=TR,c3=BR,c4=BL.
void RendererES2::pushSkew(float x1, float y1, float x2, float y2,
                        float x3, float y3, float x4, float y4,
                        uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4) {
    float W = (float)width_, H = (float)height_;
    SkewInstance s;
    // Convert pixel (x,y_down) → normalised (x/W, (H-y)/H)
    s.tlX = x1/W; s.tlY = (H-y1)/H;
    s.trX = x2/W; s.trY = (H-y2)/H;
    s.brX = x3/W; s.brY = (H-y3)/H;
    s.blX = x4/W; s.blY = (H-y4)/H;
    s.cTL = c1; s.cTR = c2; s.cBR = c3; s.cBL = c4;
    skews_.push_back(s);
}

// ---- flush helpers ----------------------------------------------------------

void RendererES2::flushGrads() {
    if (grads_.empty()) return;
    drawInstanced(gradProg_, grads_.data(), (int)grads_.size(),
                  sizeof(GradInstance), kGradAttrs, 5, gradInstVbo_);
    grads_.clear();
}

void RendererES2::flushSkews() {
    if (skews_.empty()) return;
    drawInstanced(skewProg_, skews_.data(), (int)skews_.size(),
                  sizeof(SkewInstance), kSkewAttrs, 8, skewInstVbo_);
    skews_.clear();
}

// ---- text helpers -----------------------------------------------------------

void RendererES2::drawString(const char* str, float px, float py,
                          uint32_t color, float scale) {
    glyphScale_ = scale;
    float glyphW = kGlyphW * scale;
    float cx = px;
    for (const char* p = str; *p; p++) {
        int c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E) { cx += glyphW; continue; }
        int idx = c - 0x20;
        float u = (float)idx / (float)kNumGlyphs;
        glyphs_.push_back({ cx, py, u, color });
        cx += kGlyphAdvance[idx] * scale;
    }
}

void RendererES2::flushGlyphs() {
    if (glyphs_.empty()) return;

    // Program uniforms + atlas bind persist on the program/texture-unit state, so
    // they can be set before drawInstanced (which re-binds the same program).
    glUseProgram(textProg_);
    glUniform2f(glGetUniformLocation(textProg_, "uViewport"),
                (float)width_, (float)height_);
    glUniform2f(glGetUniformLocation(textProg_, "uGlyphPx"),
                kGlyphW * glyphScale_, kGlyphH * glyphScale_);
    glUniform1f(glGetUniformLocation(textProg_, "uAtlasUStep"),
                1.0f / (float)kNumGlyphs);
    glUniform1i(glGetUniformLocation(textProg_, "uAtlas"), 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fontTex_);

    drawInstanced(textProg_, glyphs_.data(), (int)glyphs_.size(),
                  sizeof(GlyphInstance), kGlyphAttrs, 3, glyphVbo_);
    glyphs_.clear();
}

// ---- keyboard rendering (PFA RenderKeys 1:1 port) ---------------------------

static inline float pfaFloor(float v) { return floorf(v); }
static inline float pfaMax(float a, float b) { return a > b ? a : b; }

void RendererES2::renderKeyboard(const uint32_t keyColor[128]) {
    const float W = (float)width_, H = (float)height_;
    const float SharpRatio = 0.65f;

    // PFA KBPercent = 0.25 → keyboard occupies bottom 25% of screen height.
    // In PFA: fKeysY = m_fNotesY + m_fNotesCY = height of notes area top (in screen pixels, y down).
    // In aPFA: keyboard is at the bottom. fKeysY_px = top of keyboard in pixel-y-down coords.
    float fKeysCY  = H * kbFrac_;
    float fKeysY   = H - fKeysCY;   // top of keyboard in screen px (y downward)

    // PFA proportions — exactly from GameState.cpp
    float fTransitionPct = 0.02f;
    float fTransitionCY  = pfaMax(3.0f, pfaFloor(fKeysCY * fTransitionPct + 0.5f));
    float fRedPct        = 0.05f;
    float fRedCY         = pfaFloor(fKeysCY * fRedPct + 0.5f);
    float fSpacerCY      = 2.0f;
    float fTopCY         = pfaFloor((fKeysCY - fSpacerCY - fRedCY - fTransitionCY) * 0.95f + 0.5f);
    float fNearCY        = fKeysCY - fSpacerCY - fRedCY - fTransitionCY - fTopCY;

    // PFA colors — exactly from GameState.cpp InitScreen()
    // SetColor(iColor_BGR, dDark, dVeryDark)
    KbColor csKBBackground = makeKbColor(0x00999999, 0.4f, 0.0f);
    KbColor csBackground   = makeKbColor(0x00464646, 0.7f, 1.3f);
    KbColor csKBRed        = makeKbColor(0x000D0A98, 0.5f, 0.2f);
    KbColor csKBWhite      = makeKbColor(0x00FFFFFF, 0.8f, 0.6f);
    KbColor csKBSharp      = makeKbColor(0x00404040, 0.5f, 0.0f);

    // White key width in pixels (for gap calc)
    float fWhiteCX = keyW_[0] * W;   // key 0 is white (C-1); same formula as PFA
    // Actually use the first white key in range
    for (int k = startNote_; k <= endNote_; k++) {
        if (!pfaIsSharp(k)) { fWhiteCX = keyW_[k] * W; break; }
    }

    // PFA gap calc
    float fKeyGap  = pfaMax(1.0f, pfaFloor(fWhiteCX * 0.05f + 0.5f));
    float fKeyGap1 = fKeyGap - pfaFloor(fKeyGap / 2.0f + 0.5f);

    // Sharp key proportions
    float fSharpCY  = fTopCY * 0.67f;
    float fSharpTop = SharpRatio * 0.7f;

    // fNearCY for 3D skew bottom of sharps
    // (used as the "perspective" depth on the bottom edge of sharps)

    // -----------------------------------------------------------------------
    // Draw background layers (PFA order)
    // -----------------------------------------------------------------------

    // 1. Solid very-dark background covering whole KB area
    pushFlatRect(0.0f, fKeysY, W, fKeysCY, csKBBackground.veryDark);

    // 2. Transition gradient (notes bg → KB bg) at top of keyboard
    pushGradRect(0.0f, fKeysY, W, fTransitionCY,
        csBackground.primary, csBackground.primary,
        csKBBackground.veryDark, csKBBackground.veryDark);

    // 3. Red stripe
    pushGradRect(0.0f, fKeysY + fTransitionCY, W, fRedCY,
        csKBRed.dark, csKBRed.dark,
        csKBRed.primary, csKBRed.primary);

    // 4. Spacer
    pushFlatRect(0.0f, fKeysY + fTransitionCY + fRedCY, W, fSpacerCY,
        csKBBackground.dark);

    flushGrads();

    // y where actual key bodies start (pixel, y-downward)
    float fCurY = fKeysY + fTransitionCY + fRedCY + fSpacerCY;

    // -----------------------------------------------------------------------
    // Draw white keys
    // -----------------------------------------------------------------------
    {
        int iStartRender = (pfaIsSharp(startNote_) ? startNote_ - 1 : startNote_);
        int iEndRender   = (pfaIsSharp(endNote_)   ? endNote_   + 1 : endNote_);
        float fStartX    = (pfaIsSharp(startNote_) ? fWhiteCX * (SharpRatio / 2.0f - 1.0f) : 0.0f);

        // We use pixel-space x for key positions to match PFA exactly
        // keyX_[k] is normalised → convert to pixels
        // Actually re-derive pixel x from PFA's white-key counting (same as layoutKeyboard)
        int allWhite = pfaWhiteCount(startNote_, endNote_ + 1);
        float fBuf = (pfaIsSharp(startNote_) ? SharpRatio/2.0f : 0.0f) +
                     (pfaIsSharp(endNote_)   ? SharpRatio/2.0f : 0.0f);
        float fWhiteCX_n = 1.0f / (allWhite + fBuf);  // normalised white key width

        float fCurX = fStartX;   // pixel offset from left edge
        float fNotesX = 0.0f;    // notes area left (full screen width)

        for (int i = iStartRender; i <= iEndRender; i++) {
            if (pfaIsSharp(i)) continue;

            bool isPressed = (keyColor[i] != 0);
            float x = fNotesX + fCurX;

            if (!isPressed) {
                // PFA white key at rest: 3-rect draw
                // Rect 1: main body gradient
                pushGradRect(x + fKeyGap1, fCurY,
                             fWhiteCX - fKeyGap, fTopCY + fNearCY,
                             csKBWhite.dark, csKBWhite.dark,
                             csKBWhite.primary, csKBWhite.primary);
                // Rect 2: bottom near section (veryDark)
                pushGradRect(x + fKeyGap1, fCurY + fTopCY,
                             fWhiteCX - fKeyGap, fNearCY,
                             csKBWhite.dark, csKBWhite.dark,
                             csKBWhite.veryDark, csKBWhite.veryDark);
                // Rect 3: top-of-near blend line
                pushGradRect(x + fKeyGap1, fCurY + fTopCY,
                             fWhiteCX - fKeyGap, 2.0f,
                             csKBBackground.dark, csKBBackground.dark,
                             csKBWhite.veryDark, csKBWhite.veryDark);

                // C4 middle-C indicator box
                if (i == 60 /* MIDI C4 */) {
                    float fMXGap = pfaFloor(fWhiteCX * 0.25f + 0.5f);
                    float fMCX   = fWhiteCX - fMXGap * 2.0f - fKeyGap;
                    float fMY    = pfaMax(fCurY + fTopCY - fMCX - 5.0f,
                                          fCurY + fSharpCY + 5.0f);
                    pushFlatRect(x + fKeyGap1 + fMXGap, fMY,
                                 fMCX, fCurY + fTopCY - 5.0f - fMY,
                                 csKBWhite.dark);
                }
            } else {
                // Pressed white key: PFA draws with track colour, no veryDark near
                KbColor csTrack = makeActiveColor(keyColor[i]);
                // Main body
                pushGradRect(x + fKeyGap1, fCurY,
                             fWhiteCX - fKeyGap, fTopCY + fNearCY - 2.0f,
                             csTrack.dark, csTrack.dark,
                             csTrack.primary, csTrack.primary);
                // Bottom 2px
                pushFlatRect(x + fKeyGap1, fCurY + fTopCY + fNearCY - 2.0f,
                             fWhiteCX - fKeyGap, 2.0f, csTrack.dark);

                // C4 indicator box (pressed version)
                if (i == 60) {
                    float fMXGap = pfaFloor(fWhiteCX * 0.25f + 0.5f);
                    float fMCX   = fWhiteCX - fMXGap * 2.0f - fKeyGap;
                    float fMY    = pfaMax(fCurY + fTopCY + fNearCY - fMCX - 7.0f,
                                          fCurY + fSharpCY + 5.0f);
                    // Ghost white box under colour
                    pushFlatRect(x + fKeyGap1 + fMXGap, fMY,
                                 fMCX, fCurY + fTopCY + fNearCY - 7.0f - fMY,
                                 csKBWhite.dark);
                    // Coloured box on top
                    pushFlatRect(x + fKeyGap1 + fMXGap, fMY,
                                 fMCX, fCurY + fTopCY + fNearCY - 7.0f - fMY,
                                 csTrack.dark);
                }
            }

            // Right gap shadow (divider between white keys)
            pushGradRect(pfaFloor(x + fKeyGap1 + fWhiteCX - fKeyGap + 0.5f),
                         fCurY, fKeyGap, fTopCY + fNearCY,
                         csKBBackground.veryDark, csKBBackground.primary,
                         csKBBackground.primary,  csKBBackground.veryDark);

            fCurX += fWhiteCX;
        }
    }
    flushGrads();

    // -----------------------------------------------------------------------
    // Draw sharp keys (3D trapezoid using skew quads — exact PFA DrawSkew port)
    // -----------------------------------------------------------------------
    {
        int iStartRender = (startNote_ != 21 && !pfaIsSharp(startNote_) &&
                            startNote_ > 0 && pfaIsSharp(startNote_ - 1)
                            ? startNote_ - 1 : startNote_);
        int iEndRender   = (endNote_ != 108 && !pfaIsSharp(endNote_) &&
                            endNote_ < 127 && pfaIsSharp(endNote_ + 1)
                            ? endNote_ + 1 : endNote_);

        int allWhite = pfaWhiteCount(startNote_, endNote_ + 1);
        float fBuf = (pfaIsSharp(startNote_) ? SharpRatio/2.0f : 0.0f) +
                     (pfaIsSharp(endNote_)   ? SharpRatio/2.0f : 0.0f);
        float fWhiteCX_n = 1.0f / (allWhite + fBuf);

        float fStartX_sharp = (pfaIsSharp(startNote_) ? fWhiteCX * SharpRatio / 2.0f : 0.0f);
        float fCurX = fStartX_sharp;

        // PFA sharp body dimensions
        float cx_sharp = fWhiteCX * SharpRatio;   // full width of sharp
        float fSharpTopFrac = SharpRatio * 0.7f;   // width of top face as fraction of white key width

        for (int i = iStartRender; i <= iEndRender; i++) {
            if (!pfaIsSharp(i)) {
                fCurX += fWhiteCX;
                continue;
            }

            // Per-key horizontal nudge (real piano layout)
            float fNudgeX = 0.0f;
            PfaNote eNote = pfaNoteVal(i);
            if      (eNote == CS_ || eNote == FS_) fNudgeX = -SharpRatio / 5.0f;
            else if (eNote == AS_ || eNote == DS_) fNudgeX =  SharpRatio / 5.0f;

            float x        = fCurX - fWhiteCX * (SharpRatio / 2.0f - fNudgeX);
            float cx       = cx_sharp;
            float fSTopX1  = x + fWhiteCX * (SharpRatio - fSharpTopFrac) / 2.0f;
            float fSTopX2  = fSTopX1 + fWhiteCX * fSharpTopFrac;

            bool isPressed = (keyColor[i] != 0);

            if (!isPressed) {
                // PFA resting sharp: 6 quads
                // 1. Bottom trapezoid face
                pushSkew(fSTopX1, fCurY + fSharpCY - fNearCY,
                         fSTopX2, fCurY + fSharpCY - fNearCY,
                         x + cx,  fCurY + fSharpCY,
                         x,       fCurY + fSharpCY,
                         csKBSharp.primary, csKBSharp.primary,
                         csKBSharp.veryDark, csKBSharp.veryDark);
                // 2. Left trapezoid face
                pushSkew(fSTopX1, fCurY - fNearCY,
                         fSTopX1, fCurY + fSharpCY - fNearCY,
                         x,       fCurY + fSharpCY,
                         x,       fCurY,
                         csKBSharp.primary, csKBSharp.primary,
                         csKBSharp.veryDark, csKBSharp.veryDark);
                // 3. Right trapezoid face
                pushSkew(fSTopX2, fCurY + fSharpCY - fNearCY,
                         fSTopX2, fCurY - fNearCY,
                         x + cx,  fCurY,
                         x + cx,  fCurY + fSharpCY,
                         csKBSharp.primary, csKBSharp.primary,
                         csKBSharp.veryDark, csKBSharp.veryDark);
                // 4. Top face (flat rect) — very dark
                pushFlatRect(fSTopX1, fCurY - fNearCY,
                             fSTopX2 - fSTopX1, fSharpCY, csKBSharp.veryDark);
                // 5. Top highlight gradient (upper part of top face)
                pushSkew(fSTopX1, fCurY - fNearCY,
                         fSTopX2, fCurY - fNearCY,
                         fSTopX2, fCurY - fNearCY + fSharpCY * 0.45f,
                         fSTopX1, fCurY - fNearCY + fSharpCY * 0.35f,
                         csKBSharp.dark, csKBSharp.dark,
                         csKBSharp.primary, csKBSharp.primary);
                // 6. Top mid gradient (lower part of top face)
                pushSkew(fSTopX1, fCurY - fNearCY + fSharpCY * 0.35f,
                         fSTopX2, fCurY - fNearCY + fSharpCY * 0.45f,
                         fSTopX2, fCurY - fNearCY + fSharpCY * 0.65f,
                         fSTopX1, fCurY - fNearCY + fSharpCY * 0.55f,
                         csKBSharp.primary, csKBSharp.primary,
                         csKBSharp.veryDark, csKBSharp.veryDark);
            } else {
                // PFA pressed sharp — two layers (matching GameState.cpp exactly):
                // Layer 1: gray base (m_csKBSharp) with reduced depth fNewNear (same 6 quads as resting)
                // Layer 2: track-color overlay drawn on top
                KbColor csTrack = makeActiveColor(keyColor[i]);
                float fNewNear = fNearCY * 0.25f;   // pressed key has shallower 3-D bevel

                // --- Layer 1: gray base (m_csKBSharp colors, fNewNear depth) ---
                pushSkew(fSTopX1, fCurY + fSharpCY - fNewNear,
                         fSTopX2, fCurY + fSharpCY - fNewNear,
                         x + cx,  fCurY + fSharpCY,
                         x,       fCurY + fSharpCY,
                         csKBSharp.primary, csKBSharp.primary,
                         csKBSharp.veryDark, csKBSharp.veryDark);
                pushSkew(fSTopX1, fCurY - fNewNear,
                         fSTopX1, fCurY + fSharpCY - fNewNear,
                         x,       fCurY + fSharpCY,
                         x,       fCurY,
                         csKBSharp.primary, csKBSharp.primary,
                         csKBSharp.veryDark, csKBSharp.veryDark);
                pushSkew(fSTopX2, fCurY + fSharpCY - fNewNear,
                         fSTopX2, fCurY - fNewNear,
                         x + cx,  fCurY,
                         x + cx,  fCurY + fSharpCY,
                         csKBSharp.primary, csKBSharp.primary,
                         csKBSharp.veryDark, csKBSharp.veryDark);
                pushFlatRect(fSTopX1, fCurY - fNewNear,
                             fSTopX2 - fSTopX1, fSharpCY, csKBSharp.veryDark);
                pushSkew(fSTopX1, fCurY - fNewNear,
                         fSTopX2, fCurY - fNewNear,
                         fSTopX2, fCurY - fNewNear + fSharpCY * 0.35f,
                         fSTopX1, fCurY - fNewNear + fSharpCY * 0.25f,
                         csKBSharp.dark, csKBSharp.dark,
                         csKBSharp.primary, csKBSharp.primary);
                pushSkew(fSTopX1, fCurY - fNewNear + fSharpCY * 0.25f,
                         fSTopX2, fCurY - fNewNear + fSharpCY * 0.35f,
                         fSTopX2, fCurY - fNewNear + fSharpCY * 0.75f,
                         fSTopX1, fCurY - fNewNear + fSharpCY * 0.65f,
                         csKBSharp.primary, csKBSharp.primary,
                         csKBSharp.veryDark, csKBSharp.veryDark);

                // --- Layer 2: track-color overlay (csTrack = channel color) ---
                // Sides: primary→dark
                pushSkew(fSTopX1, fCurY + fSharpCY - fNewNear,
                         fSTopX2, fCurY + fSharpCY - fNewNear,
                         x + cx,  fCurY + fSharpCY,
                         x,       fCurY + fSharpCY,
                         csTrack.primary, csTrack.primary,
                         csTrack.dark, csTrack.dark);
                pushSkew(fSTopX1, fCurY - fNewNear,
                         fSTopX1, fCurY + fSharpCY - fNewNear,
                         x,       fCurY + fSharpCY,
                         x,       fCurY,
                         csTrack.primary, csTrack.primary,
                         csTrack.dark, csTrack.dark);
                pushSkew(fSTopX2, fCurY + fSharpCY - fNewNear,
                         fSTopX2, fCurY - fNewNear,
                         x + cx,  fCurY,
                         x + cx,  fCurY + fSharpCY,
                         csTrack.primary, csTrack.primary,
                         csTrack.dark, csTrack.dark);
                // Top face flat (track dark)
                pushFlatRect(fSTopX1, fCurY - fNewNear,
                             fSTopX2 - fSTopX1, fSharpCY, csTrack.dark);
                // Top highlight: flat primary (all 4 corners same — no gradient)
                pushSkew(fSTopX1, fCurY - fNewNear,
                         fSTopX2, fCurY - fNewNear,
                         fSTopX2, fCurY - fNewNear + fSharpCY * 0.35f,
                         fSTopX1, fCurY - fNewNear + fSharpCY * 0.25f,
                         csTrack.primary, csTrack.primary,
                         csTrack.primary, csTrack.primary);
                // Top mid gradient (primary→dark)
                pushSkew(fSTopX1, fCurY - fNewNear + fSharpCY * 0.25f,
                         fSTopX2, fCurY - fNewNear + fSharpCY * 0.35f,
                         fSTopX2, fCurY - fNewNear + fSharpCY * 0.75f,
                         fSTopX1, fCurY - fNewNear + fSharpCY * 0.65f,
                         csTrack.primary, csTrack.primary,
                         csTrack.dark, csTrack.dark);
            }
        }
    }
    flushGrads();
    flushSkews();
}

// ---- main render ------------------------------------------------------------

void RendererES2::render(float clockSec, float totalSec, float fps,
                      float windowSec,
                      const std::vector<NoteInstance>& notes,
                      const uint32_t keyColor[128]) {
    if (!valid()) return;

#if defined(__APPLE__)
    // EAGL has no default framebuffer 0 tied to the screen — bind the
    // CAEAGLLayer-backed FBO before any draw. (No-op-equivalent on Android,
    // which renders straight to the window surface's framebuffer 0.)
    eaglBindDrawable(glSurface_);
#endif

    // Keyboard height: match PFA's aspect-ratio-driven approach (GameState.cpp ~line 1969).
    // PFA constants: KBPercent = 0.25, KeyRatio = 0.1775
    //   fMaxKeyCY  = height * KBPercent                  (hard cap)
    //   fIdealKeyCY = fWhiteCX / KeyRatio                (derived from key width → correct aspect ratio)
    //   fIdealKeyCY = (fIdealKeyCY / 0.95 + 2.0) / 0.93 (PFA's internal proportion adjustment)
    //   keyboard height = min(fIdealKeyCY, fMaxKeyCY)
    // This prevents vertical stretching when keys are narrow (e.g. portrait or small screens).
    {
        const float KBPercent = 0.25f;
        const float KeyRatio  = 0.1775f;

        // Compute white key width in pixels (same formula as layoutKeyboard / renderKeyboard)
        const float SharpRatioKb = 0.65f;
        int allWhiteKb = pfaWhiteCount(startNote_, endNote_ + 1);
        float fBufKb = (pfaIsSharp(startNote_) ? SharpRatioKb / 2.0f : 0.0f)
                     + (pfaIsSharp(endNote_)   ? SharpRatioKb / 2.0f : 0.0f);
        float fWhiteCX_kb = (float)width_ / (allWhiteKb + fBufKb);

        float fMaxKeyCY   = (float)height_ * KBPercent;
        float fIdealKeyCY = fWhiteCX_kb / KeyRatio;
        // PFA's internal proportion adjustment (accounts for spacer, red strip, transition)
        fIdealKeyCY = (fIdealKeyCY / 0.95f + 2.0f) / 0.93f;

        float kbPx = fIdealKeyCY < fMaxKeyCY ? fIdealKeyCY : fMaxKeyCY;
        kbFrac_ = (float)height_ > 0 ? kbPx / (float)height_ : KBPercent;
    }

    glViewport(0, 0, width_, height_);

    // Background: use configurable bgColor_ (PFA BGR format → linear RGB for GL)
    {
        float R = ((bgColor_ >>  0) & 0xFF) / 255.0f;
        float G = ((bgColor_ >>  8) & 0xFF) / 255.0f;
        float B = ((bgColor_ >> 16) & 0xFF) / 255.0f;
        glClearColor(R, G, B, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    // Note field geometry (pixels, y-downward convention matching PFA)
    const float W = (float)width_, H = (float)height_;
    float fNotesCY = H * (1.0f - kbFrac_);  // height of note field
    float fNotesY  = 0.0f;                   // top of note field (y-downward)
    float fNotesCX = W;

    // ---- note field background: stretched user image, or PFA solid fill + lines ----
    if (hasBgImage_) {
        // One quad stretched across the note field (aspect not preserved). The
        // octave-split lines are skipped so the image reads cleanly behind notes.
        glUseProgram(bgProg_);
        glUniform1f(glGetUniformLocation(bgProg_, "uYBottom"), 2.0f * kbFrac_ - 1.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bgTex_);
        glUniform1i(glGetUniformLocation(bgProg_, "uTex"), 0);
        // Plain (non-instanced) unit quad — bgProg only reads aQuad (location 0).
        // glDrawArrays on a triangle strip is core ES 2.0, so no instancing needed.
        for (GLuint i = 0; i <= 8; i++) glDisableVertexAttribArray(i);
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        // Solid background fill for note field
        KbColor csBackground = makeKbColor(bgColor_, 0.7f, 1.3f);
        pushFlatRect(0.0f, fNotesY, W, fNotesCY, csBackground.primary);

        // Vertical lines: draw at boundaries between adjacent white keys (PFA RenderLines exact)
        const float SharpRatio = 0.65f;
        int allWhite = pfaWhiteCount(startNote_, endNote_ + 1);
        float fBuf = (pfaIsSharp(startNote_) ? SharpRatio/2.0f : 0.0f) +
                     (pfaIsSharp(endNote_)   ? SharpRatio/2.0f : 0.0f);
        float fWhiteCX = W / (allWhite + fBuf);

        for (int i = startNote_ + 1; i <= endNote_; i++) {
            if (!pfaIsSharp(i - 1) && !pfaIsSharp(i)) {
                int iWhiteKeys = pfaWhiteCount(startNote_, i);
                float fStartX = pfaIsSharp(startNote_) ? SharpRatio / 2.0f : 0.0f;
                float x = fWhiteCX * (iWhiteKeys + fStartX);
                x = floorf(x + 0.5f);
                // PFA: 3px wide gradient (dark, veryDark, veryDark, dark) — left-to-right
                pushGradRect(x - 1.0f, fNotesY, 3.0f, fNotesCY,
                    csBackground.dark,     csBackground.veryDark,
                    csBackground.veryDark, csBackground.dark);
            }
        }
        flushGrads();
    }

    // ---- note field — whites first, then sharps on top (PFA layering) ----
    if (!notes.empty()) {
        glUseProgram(noteProg_);
        glUniform1f(glGetUniformLocation(noteProg_, "uClockSec"), clockSec);
        glUniform1f(glGetUniformLocation(noteProg_, "uWindowSec"),
                    windowSec > 1e-6f ? windowSec : 1e-6f);
        glUniform1f(glGetUniformLocation(noteProg_, "uKbFrac"), kbFrac_);
        glUniform2f(glGetUniformLocation(noteProg_, "uViewportPx"),
                    (float)width_, (float)height_);
        float whiteKeyPx = 0.0f;
        for (int k = startNote_; k <= endNote_; k++) {
            if (!pfaIsSharp(k)) { whiteKeyPx = keyW_[k] * (float)width_; break; }
        }
        glUniform1f(glGetUniformLocation(noteProg_, "uWhiteKeyPx"), whiteKeyPx);

        // Fill each instance's keyX/keyW from the key layout (replaces the old
        // dynamically-indexed uKey[] uniform — see kNoteVS). Folds into the
        // existing white/sharp split copy, so no extra per-note pass.
        auto fillSplit = [&](uint32_t wantSharp) {
            notesScratch_.clear();
            for (const auto& n : notes) {
                if (n.isSharp != wantSharp) continue;
                int k = (int)(n.key + 0.5f);
                k = k < 0 ? 0 : (k > 127 ? 127 : k);
                // Widen the shared 28-byte NoteInstance into the renderer's
                // 36-byte NoteInstanceES2, filling keyX/keyW from the key layout.
                NoteInstanceES2 m;
                m.startSec      = n.startSec;
                m.durSec        = n.durSec;
                m.key           = n.key;
                m.keyX          = keyX_[k];
                m.keyW          = keyW_[k];
                m.colorPrimary  = n.colorPrimary;
                m.colorDark     = n.colorDark;
                m.colorVeryDark = n.colorVeryDark;
                m.isSharp       = n.isSharp;
                notesScratch_.push_back(m);
            }
        };

        // Pass 1: white key notes (uniforms set above persist through drawInstanced,
        // which re-binds noteProg_).
        fillSplit(0u);
        if (!notesScratch_.empty())
            drawInstanced(noteProg_, notesScratch_.data(), (int)notesScratch_.size(),
                          sizeof(NoteInstanceES2), kNoteAttrs, 7, instVbo_);

        // Pass 2: sharp key notes on top
        fillSplit(1u);
        if (!notesScratch_.empty())
            drawInstanced(noteProg_, notesScratch_.data(), (int)notesScratch_.size(),
                          sizeof(NoteInstanceES2), kNoteAttrs, 7, instVbo_);
    }

    // ---- PFA-faithful keyboard rendering ----
    renderKeyboard(keyColor);

    // ---- HUD text -- PFA RenderStatus() / RenderText() exactly ----
    // PFA: top-right corner, fixed 156px wide, height = 6 + 16*iLines
    // Lines (top to bottom): Time, FPS, Score: N/A
    // Label left-aligned, value right-aligned within inner rect (InflateRect -6,-3).
    // Shadow drawn at (+2,+1) offset, white at (0,0).
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Time value string only (no "Time:" label inline) — label drawn separately left-aligned.
    // PFA uses proportional Tahoma so label+value fit in 144px; aPFA's 9px monospace font
    // is wider, so we draw label left and value right (they may coexist without overlap
    // because the value is right-aligned and starts after the label naturally in PFA).
    // To match visually: draw label left, value right — same as PFA's DT_LEFT / DT_RIGHT.
    char sTime[64], sFPSVal[32];
    {
        // PFA's 3-second pre-roll runs the clock from -3.0s up to 0; show it as
        // "-0:03.0" exactly like stock PFA (m_llStartTime = llFirstNote - 3000000).
        bool  neg = clockSec < 0.0f;
        float cs  = neg ? -clockSec : clockSec;
        float ts  = totalSec < 0.0f ? 0.0f : totalSec;
        long long cms = (long long)(cs / 60.0f);  float csec = cs - cms * 60.0f;
        long long tms = (long long)(ts / 60.0f);  float tsec = ts - tms * 60.0f;
        snprintf(sTime,   sizeof(sTime),   "%s%lld:%04.1f / %lld:%04.1f",
                 neg ? "-" : "", cms, csec, tms, tsec);
        snprintf(sFPSVal, sizeof(sFPSVal), "%.1f", fps);
    }
    const char* sScore = "N/A";  // PFA default: no MIDI input / not scored

    // Whole status box (background + text + insets) blown up 15% over PFA's
    // RenderStatus geometry, keeping every proportion. kHudScale multiplies the
    // glyph scale and every pixel dimension uniformly, so it stays a single knob.
    const float kHudScale = 1.15f;
    const float scale = kHudScale;
    const float gW    = kGlyphW * scale;
    const float lineH = 16.0f * kHudScale;

    // PFA fixed geometry (RenderText): rcStatus = { bufW-156, 0, bufW, 6+16*3 }
    // PFA uses proportional Tahoma so 156px fits; aPFA's 9px monospace needs 192px
    // so "Time:" label + right-aligned value both fit without overlap.
    // OpenGL Y=0 is BOTTOM of screen. Box sits at the top-right corner.
    const float boxW  = 192.0f * kHudScale;
    const float boxH  = 6.0f * kHudScale + lineH * 3.0f;  // 3 lines: Time, FPS, Score
    const float boxX  = (float)width_ - boxW;
    // top of box = height_, bottom of box = height_ - boxH
    const float boxTop = (float)height_;       // screen top in pixel coords

    // Background (PFA: 0x80000000) — rect shader uses normalised [0..1] Y=0=bottom
    {
        RectInstance bg;
        bg.x = boxX / (float)width_;
        bg.y = (boxTop - boxH) / (float)height_;   // bottom of box (normalised)
        bg.w = boxW / (float)width_;
        bg.h = boxH / (float)height_;
        bg.color = 0x80000000u;
        rects_.clear();
        rects_.push_back(bg);
        drawInstanced(rectProg_, rects_.data(), 1,
                      sizeof(RectInstance), kRectAttrs, 2, rectInstVbo_);
    }

    // Inner rect after PFA InflateRect(-6,-3)
    const float innerX = boxX + 6.0f * kHudScale;
    const float innerR = boxX + boxW - 6.0f * kHudScale;
    // glyph py = bottom of glyph in pixel space (Y=0=bottom).
    // Line 0 (Time) is topmost: its glyph bottom = boxTop - 3(top inset) - gH
    // Line 1 (FPS)   is 16px below Time
    // Line 2 (Score) is 16px below FPS
    const float gH    = kGlyphH * scale;
    const float line0Y = boxTop - 3.0f * kHudScale - gH;  // Time  (topmost)
    const float line1Y = line0Y - lineH;                // FPS
    const float line2Y = line1Y - lineH;                // Score

    const uint32_t kShadow   = 0xFF404040u;
    const uint32_t kWhite    = 0xFFFFFFFFu;
    // PFA shadow: OffsetRect(+2,+1) means text moves 2px right and 1px DOWN in screen space.
    // In OpenGL pixel coords (Y=0=bottom), "down" = -Y.
    const float    kShadowDX = 2.0f * kHudScale;
    const float    kShadowDY = -1.0f * kHudScale;  // negative because Y=0 is bottom

    // Use kGlyphAdvance per character — matches drawString exactly.
    auto strW = [&](const char* s) -> float {
        float w = 0.0f;
        for (const char* p = s; *p; p++) {
            int c = (unsigned char)*p;
            if (c >= 0x20 && c <= 0x7E) w += kGlyphAdvance[c - 0x20] * scale;
        }
        return w;
    };

    // All lines: label left-aligned at innerX, value right-aligned at innerR — exactly PFA.
    // Shadow pass
    drawString("Time:",  innerX + kShadowDX,                   line0Y + kShadowDY, kShadow, scale);
    drawString(sTime,    innerR - strW(sTime)  + kShadowDX,    line0Y + kShadowDY, kShadow, scale);
    drawString("FPS:",   innerX + kShadowDX,                   line1Y + kShadowDY, kShadow, scale);
    drawString(sFPSVal,  innerR - strW(sFPSVal) + kShadowDX,   line1Y + kShadowDY, kShadow, scale);
    drawString("Score:", innerX + kShadowDX,                   line2Y + kShadowDY, kShadow, scale);
    drawString(sScore,   innerR - strW(sScore)  + kShadowDX,   line2Y + kShadowDY, kShadow, scale);
    flushGlyphs();

    // White pass
    drawString("Time:",  innerX,                 line0Y, kWhite, scale);
    drawString(sTime,    innerR - strW(sTime),    line0Y, kWhite, scale);
    drawString("FPS:",   innerX,                 line1Y, kWhite, scale);
    drawString(sFPSVal,  innerR - strW(sFPSVal),  line1Y, kWhite, scale);
    drawString("Score:", innerX,                 line2Y, kWhite, scale);
    drawString(sScore,   innerR - strW(sScore),   line2Y, kWhite, scale);
    flushGlyphs();

    glDisable(GL_BLEND);

#if defined(__APPLE__)
    // glFinish() fence (so GPU cost lands in the one-frame-delayed clock, the way
    // eglSwapBuffers' vblank stall did) + presentRenderbuffer. See eagl_surface.mm.
    eaglPresent(glSurface_);
#else
    eglSwapBuffers(display_, surface_);
#endif
}

void RendererES2::destroyEGL() {
    if (fontTex_) { glDeleteTextures(1, &fontTex_); fontTex_ = 0; }
    if (bgTex_)   { glDeleteTextures(1, &bgTex_);   bgTex_ = 0; hasBgImage_ = false; }
#if defined(__APPLE__)
    if (glSurface_) { eaglDestroy(glSurface_); glSurface_ = nullptr; }
#else
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
#endif
}

// Renderer factory. RendererES2 is always available (and is the only renderer on
// iOS). On Android, the default (legacy==false) is RendererES3; the "Legacy
// Renderer (GLES 2.0)" toggle (legacy==true) forces RendererES2.
// (RendererES3 is declared via renderer_es3.h, included at file scope above.)
std::unique_ptr<IRenderer> createRenderer(bool legacy) {
#if defined(__APPLE__)
    (void)legacy;
    return std::unique_ptr<IRenderer>(new RendererES2());
#else
    if (legacy) return std::unique_ptr<IRenderer>(new RendererES2());
    return std::unique_ptr<IRenderer>(new RendererES3());
#endif
}

}  // namespace apfa
