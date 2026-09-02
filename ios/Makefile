# ios/Makefile — cross-compile the iOS app FROM LINUX, without Xcode.
#
# This is the fallback path. The primary one is .github/workflows/ios.yml, which
# builds the same sources on a free GitHub macOS runner with a real Xcode; use
# that unless you need to build offline. What you give up here:
#
#   • ibtool is macOS-only, so LaunchScreen.storyboard cannot be compiled. Run
#     `make no-launch-screen` to drop the UILaunchStoryboardName key, or commit
#     the LaunchScreen.storyboardc that a CI run produces and point
#     LAUNCH_STORYBOARDC at it. WITHOUT ONE OF THOSE the app runs letterboxed at
#     320x480 and every FPS number it reports is measured at the wrong
#     resolution — which for this project makes the build worthless.
#   • actool is likewise macOS-only, so there is no app icon. Cosmetic.
#
# What you need:
#   1. clang (any recent one; Debian's clang is fine — it cross-compiles).
#   2. cctools-port for ld64 + lipo + install_name_tool, targeting arm64-apple-darwin.
#        https://github.com/tpoechtrager/cctools-port
#   3. An iPhoneOS SDK. Apple does not ship one for Linux; extract it from an
#      Xcode.xip (an Apple ID is needed to download that) or from the
#      theos/sdks mirror. Point SDK at the .sdk directory.
#   4. ldid, to fake-sign the binary so Sideloadly has something to re-sign.
#        https://github.com/ProcursusTeam/ldid
#
#   make deps      — download BASS + BASSMIDI for iOS into ../bass24-ios etc.
#   make           — build aPFA.app
#   make ipa       — package aPFA-unsigned.ipa
#   make clean

SDK          ?= $(HOME)/sdks/iPhoneOS.sdk
CLANG        ?= clang
LD64         ?= arm64-apple-darwin20-ld
LDID         ?= ldid
TARGET       ?= arm64-apple-ios12.0
BUILD        ?= build
APP           = $(BUILD)/aPFA.app
BIN           = $(APP)/aPFA

ROOT          = ..
CPP_DIR       = $(ROOT)/app/src/main/cpp
BASS_DIR      = $(ROOT)/bass24-ios
BASSMIDI_DIR  = $(ROOT)/bassmidi24-ios
# The device slice of each xcframework (the simulator one is x86_64/arm64-sim).
BASS_FW       = $(BASS_DIR)/bass.xcframework/ios-arm64_armv7_armv7s/bass.framework
BASSMIDI_FW   = $(BASSMIDI_DIR)/bassmidi.xcframework/ios-arm64_armv7_armv7s/bassmidi.framework

# Shared engine, MINUS the two Android-only pieces. Keep this list in lockstep
# with the excludes in project.yml:
#   native-lib.cpp   JNI surface (apfa_bridge.mm replaces it)
#   renderer_es3.*   Android-only, includes <android/*> unguarded
ENGINE_SRC = \
	$(CPP_DIR)/engine.cpp \
	$(CPP_DIR)/midi_parser.cpp \
	$(CPP_DIR)/synth.cpp \
	$(CPP_DIR)/renderer.cpp \
	$(CPP_DIR)/bigalloc.cpp \
	$(CPP_DIR)/streamer.cpp

IOS_MM  = eagl_surface.mm apfa_bridge.mm
IOS_M   = main.m APFAAppDelegate.m APFAGLView.m APFAImageUtil.m \
          APFASetupViewController.m APFAPlaybackViewController.m

INCLUDES = -I$(CPP_DIR) -I$(BASS_DIR) -I$(BASSMIDI_DIR) -I.

# -O2 in every configuration, deliberately: the legit run has to reflect the
# fastest the hardware genuinely allows, and an -O0 engine would be unfaithful.
COMMON = -target $(TARGET) -isysroot $(SDK) $(INCLUDES) -O2 -fobjc-arc \
         -DGLES_SILENCE_DEPRECATION=1 -DAPFA_STREAMING=1 \
         -fvisibility=hidden -Wall
CXXFLAGS = $(COMMON) -std=c++17 -stdlib=libc++
OBJCFLAGS = $(COMMON) -fmodules -fobjc-abi-version=2

LDFLAGS = -target $(TARGET) -isysroot $(SDK) -fuse-ld=$(LD64) -stdlib=libc++ \
          -framework UIKit -framework Foundation -framework CoreGraphics \
          -framework QuartzCore -framework OpenGLES -framework AudioToolbox \
          -framework AVFoundation -framework MobileCoreServices \
          -F$(BASS_DIR)/bass.xcframework/ios-arm64_armv7_armv7s \
          -F$(BASSMIDI_DIR)/bassmidi.xcframework/ios-arm64_armv7_armv7s \
          -framework bass -framework bassmidi \
          -rpath @executable_path/Frameworks

OBJ = $(patsubst %,$(BUILD)/obj/%.o,$(notdir $(ENGINE_SRC)) $(IOS_MM) $(IOS_M))

.PHONY: all deps ipa clean no-launch-screen check

all: $(APP)

deps:
	@mkdir -p $(BASS_DIR) $(BASSMIDI_DIR)
	curl -fsSL -o /tmp/bass24-ios.zip     https://www.un4seen.com/files/bass24-ios.zip
	curl -fsSL -o /tmp/bassmidi24-ios.zip https://www.un4seen.com/files/bassmidi24-ios.zip
	unzip -oq /tmp/bass24-ios.zip     -d $(BASS_DIR)
	unzip -oq /tmp/bassmidi24-ios.zip -d $(BASSMIDI_DIR)
	@test -f $(BASS_DIR)/bass.h && test -d $(BASS_FW) && echo "BASS ok"
	@test -f $(BASSMIDI_DIR)/bassmidi.h && test -d $(BASSMIDI_FW) && echo "BASSMIDI ok"

check:
	@test -d "$(SDK)" || { echo "SDK not found at $(SDK) — set SDK=..."; exit 1; }
	@command -v $(CLANG) >/dev/null || { echo "clang not found"; exit 1; }
	@command -v $(LD64)  >/dev/null || { echo "$(LD64) not found — build cctools-port"; exit 1; }
	@test -d $(BASS_FW)  || { echo "BASS missing — run 'make deps'"; exit 1; }
	@echo "toolchain ok"

$(BUILD)/obj/%.cpp.o: $(CPP_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CLANG) $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/%.mm.o: %.mm
	@mkdir -p $(dir $@)
	$(CLANG) $(CXXFLAGS) -fobjc-arc -c $< -o $@

$(BUILD)/obj/%.m.o: %.m
	@mkdir -p $(dir $@)
	$(CLANG) $(OBJCFLAGS) -c $< -o $@

$(APP): check $(OBJ) Info.plist
	@mkdir -p $(APP)/Frameworks
	$(CLANG) $(OBJ) $(LDFLAGS) -o $(BIN)
	cp Info.plist $(APP)/Info.plist
	cp -R $(BASS_FW)     $(APP)/Frameworks/
	cp -R $(BASSMIDI_FW) $(APP)/Frameworks/
	@if [ -n "$(LAUNCH_STORYBOARDC)" ]; then \
		cp -R "$(LAUNCH_STORYBOARDC)" $(APP)/LaunchScreen.storyboardc; \
	else \
		echo ">>> WARNING: no compiled launch screen (see the header of this Makefile)."; \
		echo ">>> The app will be letterboxed at 320x480 and its FPS numbers will be"; \
		echo ">>> measured at the wrong resolution. Run 'make no-launch-screen' or"; \
		echo ">>> supply LAUNCH_STORYBOARDC=<path to LaunchScreen.storyboardc>."; \
	fi
	$(LDID) -S $(BIN)
	@echo "built $(APP)"

# Drop the launch-storyboard key from the built app's Info.plist. Note this is
# a mitigation, not a fix: without launch assets iOS 12 still letterboxes. Use
# it only to get a runnable build for a quick check, never for anything you are
# going to take timings from.
no-launch-screen:
	@python3 -c "import plistlib,sys;p='$(APP)/Info.plist';\
d=plistlib.load(open(p,'rb'));d.pop('UILaunchStoryboardName',None);\
plistlib.dump(d,open(p,'wb'));print('dropped UILaunchStoryboardName')"

ipa: $(APP)
	rm -rf $(BUILD)/Payload $(BUILD)/aPFA-unsigned.ipa
	mkdir -p $(BUILD)/Payload
	cp -R $(APP) $(BUILD)/Payload/
	cd $(BUILD) && zip -qry aPFA-unsigned.ipa Payload
	@echo "built $(BUILD)/aPFA-unsigned.ipa"

clean:
	rm -rf $(BUILD)
