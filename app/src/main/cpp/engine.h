// engine.h — the legit-run engine: one-frame-delayed clock, dispatch, render.
//
// Clock, dispatch, buildVisible, GL render, and eglSwapBuffers all run on ONE
// engine thread, pinned to the fastest core — exactly as PFA's GameThread runs
// Logic() and Render() (including Present()) back-to-back on one thread.
// The vsync stall from eglSwapBuffers enters the clock the same way D3D
// Present() enters PFA's clock, so GPU cost on slow hardware stretches the
// frame time faithfully.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "midi_parser.h"
#include "renderer.h"
#include "synth.h"
#ifdef APFA_STREAMING
#include "streamer.h"   // file-backed event pool (flag default OFF — see streamer.h)
#endif

namespace apfa {

class Engine {
public:
    ~Engine() { stop(); }

    // legacyRenderer selects the ES2 "Legacy Renderer (GLES 2.0)" path on
    // Android. allowChunked permits the streamer's chunked on-disk sort
    // ("Chunked Disk Streaming" in Advanced Settings) — it does NOT force
    // streaming: whether a load streams at all stays automatic (RAM-fit
    // prediction / crash marker), and the chunked sort is picked only when
    // the un-chunked load transient wouldn't fit either (streaming builds
    // only; ignored elsewhere). poolDir likewise only relocates the pagefile
    // if a load streams at all ("Pagefile on SD Card"); empty = next to the
    // MIDI, in the app cache dir. All defaulted so the iOS bridge's existing
    // call compiles unchanged (iOS always uses the ES2/universal renderer and
    // the in-RAM parse regardless).
    bool load(const std::string& midiPath, const std::string& soundfontPath,
              int voiceCount, float noteSpeed, uint64_t cpuMask,
              bool legacyRenderer = false, bool allowChunked = false,
              const std::string& poolDir = std::string());
    // Why load() returned false, for the UI's error message. Generic parse
    // failures stay kLoadErrNone (the caller's default message covers them).
    enum LoadError {
        kLoadErrNone      = 0,
        kLoadNeedsChunked = 1,   // needs "Chunked Disk Streaming", toggle is off
        kLoadDiskFull     = 2,   // free-space guard: pagefile would exhaust storage
        kLoadFileTooBig   = 3,   // pagefile past the volume's limit (FAT32's 4 GB)
        kLoadNoAddrSpace  = 4,   // pool's VA reservation failed (32-bit process)
        kLoadTooBigForRam = 5,   // streaming unavailable and the parse can't fit
        kLoadNoMergeRam   = 6,   // chunked, but sisterPos_+inv[] still won't fit
    };
    int loadError() const { return loadError_; }
    std::atomic<float>& loadProgress() { return loadProgress_; }

    int64_t noteCount()   const { return static_cast<int64_t>(midi_.noteCount()); }
#ifdef APFA_STREAMING
    // Streaming: midi_.memoryBytes() covers the resident tables (events[],
    // colors, pcIdx — eventPool is empty); the streamer adds its own tables
    // plus the pool pages actually resident right now (mincore).
    int64_t memoryBytes() const {
        return static_cast<int64_t>(midi_.memoryBytes() + streamer_.memoryBytes());
    }
    // Bytes of pool streamed through the disk pagefile for this load; 0 when
    // the plain in-RAM parse was used (small MIDIs) — the UI shows the
    // "(N MB streamed)" suffix only in that nonzero case.
    int64_t streamedBytes() const { return static_cast<int64_t>(streamer_.diskBytes()); }
#else
    int64_t memoryBytes() const { return static_cast<int64_t>(midi_.memoryBytes()); }
    int64_t streamedBytes() const { return 0; }
#endif

    // Opaque native surface handle. Android: ANativeWindow*. iOS: CAEAGLLayer*.
    void start(void* surface);
    void stop();
    void surfaceChanged(int w, int h);
    void pause()  { paused_ = true; }
    void resume() { paused_ = false; }
    void seek(int64_t micros);
    void setBgColor(uint32_t bgrColor) { bgColor_.store(bgrColor); }
    // Stretched background image, set from the UI thread. rgba = w*h*4 bytes, row
    // 0 = top. The render thread uploads it to GL on the next frame. Empty/0 drops
    // back to the solid bgColor.
    void setBgImage(const uint8_t* rgba, int w, int h);

    bool    isPlaying() const { return playing_.load(); }
    int64_t timeUs()    const { return pubTimeUs_.load(); }
    int64_t totalUs()   const { return midi_.totalUs; }
    // Seek bounds, mirroring PFA's MainScreen::GetMinTime/GetMaxTime
    // (GameState.h:276-277). The position bar spans [minTimeUs, maxTimeUs], so
    // scrubbing to the start lands in the 3-second pre-roll (the "-0:03" clock),
    // exactly like stock PFA — not at 0:00.
    int64_t minTimeUs() const { return firstNoteUs_ - 3000000; }
    int64_t maxTimeUs() const { return midi_.totalUs + 500000; }
    float   fps()       const { return pubFps_.load(); }

    // Start-up failure code, surfaced to the UI so an init failure shows a real
    // message instead of an infinite "Starting…". 0 = none/still starting/ok.
    enum StartError { kStartOk = 0, kStartErrSynth = 1, kStartErrRenderer = 2 };
    int startError() const { return startError_.load(); }

private:
    void threadMain();
    void frame();
    void dispatch();
    void buildVisible();
    void applySeek(int64_t target);
    // events[pos]'s time without dereferencing it — a sliced streaming load
    // maps only one slice at a time (streamer.h).
    int64_t eventUsAt(size_t pos) const;
    void advancePcCursor();
    void playSkippedEvents(size_t oldPcCursor);

    MidiData midi_;
#ifdef APFA_STREAMING
    // Owns the pool file mapping midi_.events points into, so it must outlive
    // any use of midi_ — both are engine members, destroyed together.
    Streamer streamer_;
    // Adaptive-load crash marker (engine.cpp): survives an lmkd kill during
    // the in-RAM parse; its presence flips the SAME MIDI to the streaming
    // pool next time. Cleared once playback starts / on clean teardown.
    void clearLoadMarker();
    std::string markerPath_;
    bool markerArmed_ = false;
#endif
    Synth    synth_;
    std::unique_ptr<IRenderer> renderer_;   // ES3 (default) or ES2 (legacy); created in threadMain
    std::string sfPath_;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> playing_{false};
    std::atomic<int>  startError_{kStartOk};
    // INT64_MIN = "no pending seek". A plain -1 won't do: PFA's pre-roll makes
    // negative targets (down to firstNoteUs_ - 3,000,000) legitimate seek values.
    std::atomic<int64_t> seekRequest_{INT64_MIN};
    std::atomic<int>  surfW_{0}, surfH_{0};
    void*             window_ = nullptr;   // ANativeWindow* (Android) / CAEAGLLayer* (iOS)

    int      loadError_  = kLoadErrNone;   // set by load() on failure (see enum)
    int      voiceCount_ = 250;
    float    noteSpeed_  = 0.05f;
    uint64_t cpuMask_    = 0;
    uint64_t pinnedMask_ = 0;   // resolved engine pin, re-asserted every 500 ms
    bool     legacyRenderer_ = false;   // ES2 legacy renderer toggle (Android)
    int64_t  firstNoteUs_    = 0;       // time of the first note-on; pre-roll = this - 3,000,000
    std::atomic<uint32_t> bgColor_{0x00464646u};

    // Pending background image — filled by the UI thread, drained (uploaded to GL)
    // by the render thread. Guarded because the two threads touch it.
    std::mutex            bgImgMutex_;
    std::vector<uint8_t>  bgImgPixels_;
    int                   bgImgW_ = 0, bgImgH_ = 0;
    bool                  bgImgDirty_ = false;
    void syncBgImage();   // render-thread: upload pending image if dirty

    // one-frame-delayed clock. Signed: PFA's 3-second pre-roll starts it at
    // firstNoteUs_ - 3,000,000 (negative until the first note is reached).
    int64_t  clockUs_  = 0;
    uint64_t lastWall_ = 0;

    // Surface re-attach support. stop() saves the current clock here; the next
    // start()'s threadMain seeks back to it instead of restarting at the pre-roll,
    // so backgrounding to recents and returning resumes in place.
    bool     hasResume_ = false;
    int64_t  resumeUs_  = 0;

    // playback cursors
    size_t eventCursor_  = 0;
    size_t windowCursor_ = 0;
    size_t pcCursor_     = 0;
    std::vector<int> active_;
    int noteState_[128] = {0};

    // render scratch — built and consumed inline on the engine thread
    std::vector<NoteInstance> instances_;
    uint32_t keyColor_[128] = {0};

    // metrics
    std::atomic<float>   loadProgress_{0.0f};
    std::atomic<int64_t> pubTimeUs_{0};
    std::atomic<float>   pubFps_{0.0f};
    int      fpsFrames_ = 0;
    uint64_t fpsLastUs_ = 0;
    uint64_t cpuLastUs_ = 0;
    uint64_t sumDispatchUs_ = 0, sumBuildUs_ = 0;
    uint64_t maxDispatchUs_ = 0, maxBuildUs_ = 0;
    // The "state:" and "fault:" lines. state: is the twin of PFA's PerfLog
    // state: line (winport/PerfLog.h) so an aPFA log and a desktop PFA log of
    // the same MIDI line up column for column. fault: has no PFA twin — desktop
    // PFA is always RAM-resident, while a streaming load runs the O(P) scan
    // against a file-backed pool, and a major fault there costs ~100 us against
    // the ~100 ns the DRAM miss would otherwise have been. Engine faults and
    // loader faults are counted separately: loader faults are read-ahead doing
    // its job, engine faults are the scan reaching an unpinned page in the hot
    // path.
    uint64_t polySum_    = 0;    // sum of active_.size() over the window's frames
    uint64_t polyMax_    = 0;
    uint64_t dispEvents_ = 0;    // events consumed by dispatch() this window
    uint64_t majFltLast_ = 0;    // engine-thread major faults at the last tick
    uint64_t ldrFltLast_ = 0;    // loader-thread major faults at the last tick
};

}  // namespace apfa
