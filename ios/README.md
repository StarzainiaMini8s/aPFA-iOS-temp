# aPFA for iOS

The iOS port of aPFA. It **reuses the shared C++ engine in `../app/src/main/cpp/`
unchanged** — the legit-run logic, the BASSMIDI synth, the data model and the
renderer are the same source files the Android build compiles. Only platform
glue differs, and all of it is behind `#ifdef __APPLE__` inside those same files.

Everything in this directory is the platform layer that replaces the Android
shell.

| File | Android twin | What it does |
|---|---|---|
| `eagl_surface.mm` | the EGL block in `renderer.cpp::initEGL` | EAGL context + CAEAGLLayer-backed renderbuffer + FBO |
| `apfa_bridge.{h,mm}` | `native-lib.cpp` | flat C API over one `apfa::Engine` |
| `APFASetupViewController.m` | `MainActivity.kt` | pick MIDI/soundfont, Voice Count, Note Speed, background |
| `APFAPlaybackViewController.m` | `PlaybackActivity.kt` | loading screen → GL surface, transport, lifecycle |
| `APFAGLView.m` | the `SurfaceView` | `+layerClass` = `CAEAGLLayer`, reports pixel size |
| `APFAImageUtil.m` | `applyBgImage` | decode a background image to packed RGBA |
| `APFAAppDelegate.m`, `main.m` | the manifest's launcher activity | window + root controller |

**Status: built and run.** The macOS runner produces the `.ipa`, and it has
played `9KX2 18 Million Notes.mid` end to end on an A19 Pro — 18 M notes, in
RAM, no streaming pool involved. What has *not* run on a device is everything
added with the streaming pool below: it compiles into the target now, but no
iOS load has yet been big enough to take that path in front of anyone.

---

## Building without a Mac

### Path 1 — GitHub Actions macOS runner (recommended)

`../.github/workflows/ios.yml` builds an **unsigned `.ipa`** on a free
GitHub-hosted macOS runner. This is the only path that gets you a real Xcode, so
it is the only one that compiles `LaunchScreen.storyboard` (see the warning
below) and the only one where `xcodebuild` validates the whole bundle.

1. Push this tree to a GitHub repo **with `ios/`, `app/`, `.github/` at the
   repo root**. macOS runner minutes are free on public repos; a private repo
   spends them at a 10x multiplier, so make it public if you can.
2. Actions tab → **build-ios-ipa** → *Run workflow*.
3. Download the `aPFA-unsigned-ipa` artifact from the finished run.

The workflow installs XcodeGen, downloads BASS + BASSMIDI for iOS from un4seen,
generates `aPFA.xcodeproj` from `project.yml`, builds Release for `iphoneos` with
signing disabled, and zips the `.app` into `Payload/`.

### Path 2 — cross-compile on Linux

`Makefile` builds the same sources with clang + cctools-port + an extracted
iPhoneOS SDK. Read its header comment for the four prerequisites. Use it when
you need to build offline; it cannot compile the launch storyboard or the app
icon.

```sh
make deps          # fetch BASS + BASSMIDI into ../bass24-ios, ../bassmidi24-ios
make check         # verify SDK, clang, ld64, deps
make ipa SDK=/path/to/iPhoneOS.sdk
```

> **Launch screen warning.** `ibtool` is macOS-only. Without a compiled
> `LaunchScreen.storyboardc` iOS assumes a 320x480 app and letterboxes it, which
> silently changes the resolution the renderer is measured at — every FPS number
> from such a build is a number for the wrong screen. Either commit the
> `.storyboardc` a CI run produces and pass `LAUNCH_STORYBOARDC=...`, or accept
> that a `make no-launch-screen` build is only good for checking that it runs.

### Installing on the phone

The `.ipa` from either path is unsigned. Sign and install it with a **free Apple
ID** — no developer account:

- **Sideloadly** (Windows/macOS/Linux) — plug the phone in, drag the `.ipa`,
  enter the Apple ID. Signs the app and its embedded frameworks.
- **AltStore** — same idea, and it re-signs automatically before expiry.

A free Apple ID gives a **7-day** signing certificate, so the app stops launching
after a week and has to be re-signed. That is Apple's limit, not ours. First
launch also needs Settings → General → VPN & Device Management → trust the
developer profile.

MIDIs and soundfonts go in the app's folder: **Files → On My iPhone → aPFA**, or
over USB in Finder. The setup screen's picker works too — it copies the file into
the app container either way.

---

## Design decisions worth knowing

**Objective-C, not Swift.** The original sketch said Swift, and the no-Mac
constraint plus the iOS 12.0 floor both argue against it. The Swift 5 runtime
only shipped inside iOS from **12.2**, so a Swift app deploying to 12.0 has to
embed ~25 MB of runtime dylibs; and cross-compiling Swift from Linux needs a
Darwin Swift SDK, where Objective-C needs only clang, which every distro ships.
Objective-C costs nothing here — the shell is a few hundred lines of UIKit.

**EAGL, not Metal.** Reusing `renderer.cpp` is the whole point. A Metal renderer
would be a second renderer to keep in lockstep with PFA's drawing, and the
project already carries two.

**`glFinish()` in `eaglPresent`.** The single most important line in the port.
On Android, GPU cost enters the one-frame-delayed clock because `eglSwapBuffers`
stalls on vblank — the same way `Present()` enters PFA's clock.
`presentRenderbuffer` does not block that way, so without the `glFinish` fence a
GPU-bound frame would look free and aPFA would report a frame rate the hardware
is not achieving. Slowing down under load is the product, not a bug; this line is
what preserves it. **Verify it on-device**, against an Android run of the same
MIDI at the same settings.

**The streaming pool, and why iOS suits it.** `APFA_STREAMING` *is* defined for
this target. It used to be excluded on the grounds that "iOS has no swap", and
that reasoning was simply wrong: the pool has never used swap. It is written to
a file and then mapped `PROT_READ | MAP_PRIVATE`, so every page the engine
faults on is clean, read-only and file-backed — the one kind of page iOS can
evict *without* a swap device, and the kind that stays out of `phys_footprint`
and therefore out of jetsam's arithmetic. The design is a better fit here than
on Android, where the same pages compete with zram.

Without it the ceiling is the in-RAM parse: 72 B per `PlayEvent`, two events per
note, plus two 8 B `events[]` pointers = **160 B/note**, against a jetsam limit
near half of RAM. That is ~20 M notes on a 12 GB iPhone (the reported ceiling
from the field) and ~3–4 M on a 5S, and past it the app is killed mid-load with
no message rather than slowing down.

Five Darwin shims make the shared streamer build here, all of them inside the
shared sources:

| Linux thing | Darwin | Why it matters |
|---|---|---|
| `kPageSize = 4096` | `16384` on `__APPLE__` | arm64 kernels use 16 KB pages. At 4096 `kSegGrain` is 36864, not a multiple of 16384, and the first mapping at a pool-file boundary fails `EINVAL` several GB into the load. Rounding up is safe either way; `Streamer::open` re-checks against `sysconf` and refuses cleanly if it is ever wrong. |
| `<sys/statfs.h>` | `<sys/param.h>` + `<sys/mount.h>` | the header does not exist; and `f_type` is Darwin's own numbering, so the FAT32 guard compares `f_fstypename` instead |
| `/proc/meminfo` | `os_proc_available_memory()` | there is no `/proc`, and no system-wide "available" worth having — the loader's window has to be priced against what *this process* may still touch. Weakly imported for the iOS 12 floor, with a footprint-based estimate behind it. |
| `RUSAGE_THREAD` | — | Darwin has only `RUSAGE_SELF`; the loader's fault counter stays 0 rather than reporting the whole process |
| `QOS_CLASS_UTILITY` for the loader | `USER_INITIATED` + `IOPOL_IMPORTANT` | Darwin ties I/O priority to QoS, and UTILITY is disk-throttled. Throttling the read-ahead thread defeats the pool. |

The adaptive gate changes basis too. On Android it prices the predicted parse
against 40% of physical RAM; on iOS physical RAM is the wrong number entirely —
jetsam kills at a per-process footprint limit near half of it — so the budget is
70% of `os_proc_available_memory()`, measured before the parse allocates
anything. `Engine::load` logs both numbers once per load.

**Chunked Disk Streaming is in the setup screen.** MainActivity buries it in an
"Advanced Settings" dialog; here it is a switch in the settings column, with the
same confirmation text minus the 32-bit paragraph (this target is arm64-only, so
the pagefile is never split into 2 GB pieces). It matters more on iOS than on
Android: the un-chunked sort's RAM transient is priced against that same jetsam
limit, so the un-chunked ceiling arrives sooner here.

**No core pinning.** iOS exposes no `sched_setaffinity`. `engine.cpp` takes the
`__APPLE__` branch and puts the engine thread on `QOS_CLASS_USER_INTERACTIVE`
instead — inert on the A7 (one core type), meaningful on A10+.

**`RendererES2` always.** `createRenderer()` on Apple ignores the legacy flag and
returns `RendererES2`, which asks for an ES3 context by itself and falls back to
ES2. `RendererES3` is excluded from the target: it includes `<android/*>`
unguarded, and it would add nothing since the ES2 renderer already lights up
native instancing on an ES3 context.

---

## Known risks for the first on-device run

1. **`glFinish()` fidelity** — as above. The one timing-sensitive change.
2. **BASS re-init across background/foreground.** `apfaStop()` runs
   `Engine::stop()`, which calls `synth_.shutdown()` → `BASS_Free()`, and the
   next `apfaStart()` re-inits. The known shipped `BASS_Init` leak (error 14 is
   always secondary and masks the real audio failure until the process is
   killed) applies to that cycle here exactly as it does to Android's
   recents-and-back. If audio dies after backgrounding, that bug is the first
   suspect, not the port.
3. **`threadMajFlt()` reports 0.** Darwin has no `RUSAGE_THREAD`, so the
   `fault:` log line is always `engine 0 maj | loader 0 maj`. Deliberate — a
   process-wide count would be worse than none. Nothing else reads it, since the
   streamer is not built here.
4. **`kEAGLDrawablePropertyRetainedBacking = NO`** assumes the renderer clears
   or covers the whole field every frame. It does today.
5. **Sideloadly and embedded frameworks.** `bass.framework` and
   `bassmidi.framework` are copied into `aPFA.app/Frameworks/` and must be
   re-signed along with the executable. Sideloadly and AltStore both do this; a
   hand-rolled `ldid` signing of the main binary alone will not.
