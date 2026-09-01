# aPFA-iOS-temp

**Temporary build repo. Delete this once the iOS build is verified.**

It exists only so GitHub Actions can compile the aPFA iOS platform layer on a
macOS runner — there is no Mac on the development machine. It holds the minimum
the iOS target needs and nothing else:

- `app/src/main/cpp/` — the shared C++ engine, used **unchanged**. Already
  published as source in [`StarzainiaMini8s/aPFA`](https://github.com/StarzainiaMini8s/aPFA).
- `ios/` — the iOS platform layer (EAGL surface, C bridge, Objective-C UIKit shell).
- `.github/workflows/ios.yml` — builds an unsigned `.ipa`.

No design docs, session notes, perf logs, Android shell, or Gradle build are
here. The real tree lives elsewhere; this is a compile harness.

BASS and BASSMIDI for iOS are un4seen's and are not vendored — the workflow
downloads them from un4seen at build time.

## Build

Actions tab → **build-ios-ipa** → *Run workflow*, then download the
`aPFA-unsigned-ipa` artifact. Sign it with Sideloadly or AltStore using a free
Apple ID (7-day certificate).
