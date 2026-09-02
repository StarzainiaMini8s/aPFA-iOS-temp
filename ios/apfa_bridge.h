// apfa_bridge.h — the iOS analog of native-lib.cpp's JNI surface.
//
// One flat C API over a single process-wide apfa::Engine, so the Objective-C
// view controllers never see a C++ type. Entry point for entry point this
// mirrors Java_com_apfa_PlaybackActivity_native* — same names, same order, same
// semantics — so the two shells stay readable against each other.
//
// Threading (identical to the JNI contract): apfaLoad runs on a background
// (loading) queue; apfaGetLoadProgress may be polled from the main thread while
// it runs. Everything else is called from the main thread only, sequentially.
// The engine pointer is the one piece of cross-thread state and is atomic.
#ifndef APFA_BRIDGE_H
#define APFA_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse the MIDI and build the engine. sfPath may be "" for no soundfont (the
// setup screen warns about that, exactly as MainActivity does). allowChunked is
// the "Chunked Disk Streaming" setting. Blocking — call off the main thread.
// Note the arguments native-lib.cpp has and this does not: cpuMask (no per-core
// affinity on iOS — the engine thread takes QOS_CLASS_USER_INTERACTIVE instead,
// engine.cpp), legacyRenderer (iOS always gets RendererES2, which picks an ES3
// context by itself), and poolDir (empty here, which puts the pool beside the
// MIDI — Documents/midi in the app container, the same volume it would land on
// anyway; iOS has no removable storage to choose between).
bool apfaLoad(const char* midiPath, const char* sfPath,
              int voiceCount, float noteSpeed, bool allowChunked);

// Why the last apfaLoad returned false — apfa::Engine::LoadError. All seven
// codes are reachable on iOS now that the streaming pool is compiled in, except
// 3 (FAT32's per-file limit; the app container is always APFS) and 4 (a 32-bit
// address space; the target is arm64-only).
int      apfaGetLoadError(void);
float    apfaGetLoadProgress(void);
int64_t  apfaGetNoteCount(void);
int64_t  apfaGetMemoryBytes(void);
// Bytes of pool written to and streamed from disk for this load; 0 when the
// plain in-RAM parse was used, which is every MIDI that fits.
int64_t  apfaGetStreamedBytes(void);

// `caeaglLayer` is the CAEAGLLayer* of the playback view, where Android passes
// an ANativeWindow*. It must outlive the render thread — the view owns it, and
// apfaStop() joins the thread before the view can go away.
void     apfaStart(void* caeaglLayer);
// Surface teardown only (app backgrounded). Stops the render thread but KEEPS
// the engine and the parsed MIDI, so the next apfaStart re-attaches and resumes
// in place instead of reloading — the twin of nativeStop.
void     apfaStop(void);
// Full teardown: drop the engine and free the MIDI. Twin of nativeRelease.
void     apfaRelease(void);
void     apfaSurfaceChanged(int w, int h);

void     apfaPause(void);
void     apfaResume(void);
void     apfaSeek(int64_t micros);
bool     apfaIsPlaying(void);
// 0 = none/still starting/ok; 1 = synth failed; 2 = renderer failed.
int      apfaGetStartError(void);

int64_t  apfaGetTimeMicros(void);
int64_t  apfaGetTotalMicros(void);
// Seek-bar bounds = PFA's GetMinTime/GetMaxTime, so the far left of the slider
// lands in the 3-second pre-roll ("-0:03"), not at 0:00.
int64_t  apfaGetMinMicros(void);
int64_t  apfaGetMaxMicros(void);
float    apfaGetFps(void);

// bgrColor is PFA's BGR packing (R = bits 0-7, G = 8-15, B = 16-23).
void     apfaSetBgColor(uint32_t bgrColor);
// rgba is tightly packed w*h*4 bytes, row 0 = top. Pass NULL/0/0 to drop back
// to the solid background colour. Copied immediately; the caller may free it.
void     apfaSetBgImage(const uint8_t* rgba, int w, int h);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // APFA_BRIDGE_H
