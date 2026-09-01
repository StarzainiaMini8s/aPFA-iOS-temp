// APFAPlaybackViewController.m — see the header. Deliberately mirrors
// PlaybackActivity.kt section for section, including the comments that explain
// WHY something is done a particular way, so the two shells can be read side by
// side.
//
// Time and FPS are drawn by the native renderer every frame (RendererES2's
// drawString, PFA's RenderText() twin) — there is no UILabel for them. The seek
// slider is polled from the main thread by a CADisplayLink; it is a UI control,
// not a timing-critical element, so display-rate polling is fine.
#import "APFAPlaybackViewController.h"
#import "APFAGLView.h"
#import "APFAImageUtil.h"
#import "apfa_bridge.h"

@interface APFAPlaybackViewController () <APFAGLViewDelegate>
@end

@implementation APFAPlaybackViewController {
    NSString *_midiPath;
    NSString *_sfPath;
    int       _voiceCount;
    float     _noteSpeed;
    uint32_t  _bgColor;
    NSString *_bgImagePath;

    // loading screen
    UILabel      *_loadingLabel;
    NSTimer      *_loadingTimer;

    // playback screen
    APFAGLView   *_glView;
    UIView       *_bar;
    UISlider     *_seekBar;
    UIButton     *_pauseButton;
    UILabel      *_startingOverlay;
    CADisplayLink *_seekLink;

    NSString *_infoLine;
    BOOL      _paused;        // user-initiated pause (survives backgrounding)
    BOOL      _userSeeking;
    BOOL      _uiHidden;
    BOOL      _started;       // engine has been handed a surface at least once
    BOOL      _released;      // apfaRelease has run
}

- (instancetype)initWithMidiPath:(NSString *)midiPath
                   soundfontPath:(NSString *)soundfontPath
                      voiceCount:(int)voiceCount
                       noteSpeed:(float)noteSpeed
                         bgColor:(uint32_t)bgrColor
                     bgImagePath:(NSString *)bgImagePath {
    self = [super initWithNibName:nil bundle:nil];
    if (!self) return nil;
    _midiPath    = [midiPath copy];
    _sfPath      = [soundfontPath copy];
    _voiceCount  = voiceCount;
    _noteSpeed   = noteSpeed;
    _bgColor     = bgrColor;
    _bgImagePath = [bgImagePath copy];
    _infoLine    = @"";
    return self;
}

- (BOOL)prefersStatusBarHidden { return YES; }
// No supportedInterfaceOrientations override here on purpose: this controller
// lives inside a UINavigationController, which does NOT forward that call to
// its children, so an override would be dead code that reads as policy. The
// orientation set in Info.plist (landscape only, matching Android's
// sensorLandscape) governs both screens. Status bar appearance IS forwarded to
// the top view controller, so prefersStatusBarHidden above does take effect.

#pragma mark - lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:18/255.0 green:18/255.0
                                                 blue:24/255.0 alpha:1.0];
    // FLAG_KEEP_SCREEN_ON's twin.
    [UIApplication sharedApplication].idleTimerDisabled = YES;

    [self showLoadingScreen];

    // Surface lifecycle is deliberately kept separate from the engine's
    // lifecycle, exactly as on Android. Backgrounding tears the render thread
    // down (iOS kills an app that touches GL while suspended) but the engine and
    // its parsed MIDI live on, and returning re-attaches and resumes in place.
    // The engine is freed only when this controller is really going away.
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(appWillResignActive)
               name:UIApplicationWillResignActiveNotification object:nil];
    [nc addObserver:self selector:@selector(appDidEnterBackground)
               name:UIApplicationDidEnterBackgroundNotification object:nil];
    [nc addObserver:self selector:@selector(appWillEnterForeground)
               name:UIApplicationWillEnterForegroundNotification object:nil];
    [nc addObserver:self selector:@selector(appDidBecomeActive)
               name:UIApplicationDidBecomeActiveNotification object:nil];

    [self beginLoad];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_loadingTimer invalidate];
    [_seekLink invalidate];
    [UIApplication sharedApplication].idleTimerDisabled = NO;
    if (!_released) apfaRelease();
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    // Popped off the navigation stack for real — the twin of onDestroy. Now it
    // is safe to drop the engine and free the MIDI.
    if (self.isBeingDismissed || self.isMovingFromParentViewController) {
        [_loadingTimer invalidate]; _loadingTimer = nil;
        [_seekLink invalidate];     _seekLink = nil;
        _released = YES;
        apfaRelease();
    }
}

#pragma mark - app lifecycle -> surface lifecycle

- (void)appWillResignActive { apfaPause(); }

- (void)appDidEnterBackground {
    // Must happen before iOS suspends us: the render thread has to be joined and
    // the EAGL context torn down, or the first GL call after resume takes the
    // app down. didEnterBackground gives us seconds; apfaStop only joins a
    // thread, so this is comfortably inside that.
    if (_started) apfaStop();
}

- (void)appWillEnterForeground {
    if (_started && _glView) {
        apfaStart([_glView eaglLayerPtr]);
        apfaSurfaceChanged([_glView pixelWidth], [_glView pixelHeight]);
    }
}

- (void)appDidBecomeActive { if (!_paused) apfaResume(); }

#pragma mark - loading screen

- (void)showLoadingScreen {
    _loadingLabel = [[UILabel alloc] initWithFrame:self.view.bounds];
    _loadingLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                                     UIViewAutoresizingFlexibleHeight;
    _loadingLabel.textColor     = [UIColor whiteColor];
    _loadingLabel.font          = [UIFont systemFontOfSize:18];
    _loadingLabel.textAlignment = NSTextAlignmentCenter;
    _loadingLabel.numberOfLines = 0;
    _loadingLabel.text          = @"Loading...";
    [self.view addSubview:_loadingLabel];

    _loadingTimer = [NSTimer scheduledTimerWithTimeInterval:0.12
                                                     target:self
                                                   selector:@selector(pollLoading)
                                                   userInfo:nil
                                                    repeats:YES];
}

- (void)pollLoading {
    _loadingLabel.text = [NSString stringWithFormat:@"Loading MIDI...  %d%%",
                          (int)(apfaGetLoadProgress() * 100)];
}

- (void)beginLoad {
    NSString *midi   = _midiPath;
    NSString *sf     = _sfPath.length ? _sfPath : @"";
    int       voices = _voiceCount;
    float     speed  = _noteSpeed;

    // No soundfont means no sound. Say it once, up front, so a silent playback
    // reads as a choice rather than as a broken load.
    if (sf.length == 0) {
        [self toast:@"You are going to play this MIDI without a soundfont!"];
    }

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL ok = apfaLoad(midi.fileSystemRepresentation,
                           sf.length ? sf.fileSystemRepresentation : "",
                           voices, speed);

        // Decode + hand over the background image off the main thread (it can be
        // big). The engine just stashes it; the render thread does the GL upload.
        if (ok && self->_bgImagePath.length) {
            [APFAImageUtil withRGBAOfImageAtPath:self->_bgImagePath
                                          maxDim:1280
                                           block:^(const uint8_t *rgba, int w, int h) {
                apfaSetBgImage(rgba, w, h);
            }];
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            [self->_loadingTimer invalidate];
            self->_loadingTimer = nil;
            if (ok) {
                apfaSetBgColor(self->_bgColor);
                double mb = apfaGetMemoryBytes() / 1048576.0;
                NSNumberFormatter *f = [[NSNumberFormatter alloc] init];
                f.numberStyle = NSNumberFormatterDecimalStyle;
                NSString *notes = [f stringFromNumber:@(apfaGetNoteCount())];
                self->_infoLine = [NSString stringWithFormat:@"%@ notes  -  %.1f MB",
                                   notes, mb];
                NSLog(@"[aPFA] %@", self->_infoLine);
                [self showPlaybackScreen];
            } else {
                // Only 0 and 5 are reachable on iOS: 1-4 are all streaming-pool
                // codes and the pool is not compiled into this target.
                switch (apfaGetLoadError()) {
                    case 5:
                        [self fail:@"This MIDI is too big for this device's RAM. "
                                    "iOS has no swap, so a MIDI that does not fit "
                                    "cannot be played here at all."];
                        break;
                    default:
                        [self fail:@"Could not parse the MIDI file"];
                        break;
                }
            }
        });
    });
}

// A load that cannot proceed has something to SAY. As a transient banner fired
// while the screen was already going away, that message flashed past over a
// progress count frozen mid-update and the whole thing read as a crash. An
// alert that waits to be dismissed says the same thing where it can actually be
// read, and only then drops back to setup.
- (void)fail:(NSString *)msg {
    UIAlertController *a =
        [UIAlertController alertControllerWithTitle:@"Can't load this MIDI"
                                            message:msg
                                     preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK"
                                          style:UIAlertActionStyleDefault
                                        handler:^(UIAlertAction *action) {
        [self.navigationController popViewControllerAnimated:YES];
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)toast:(NSString *)msg {
    dispatch_async(dispatch_get_main_queue(), ^{
        UILabel *t = [[UILabel alloc] init];
        t.text = msg;
        t.numberOfLines = 0;
        t.textColor = [UIColor whiteColor];
        t.font = [UIFont systemFontOfSize:14];
        t.textAlignment = NSTextAlignmentCenter;
        t.backgroundColor = [UIColor colorWithWhite:0 alpha:0.8];
        t.layer.cornerRadius = 8;
        t.layer.masksToBounds = YES;
        CGFloat w = MIN(self.view.bounds.size.width - 40, 420);
        CGSize fit = [t sizeThatFits:CGSizeMake(w - 24, CGFLOAT_MAX)];
        t.frame = CGRectMake((self.view.bounds.size.width - w) / 2,
                             self.view.bounds.size.height - fit.height - 90,
                             w, fit.height + 20);
        t.autoresizingMask = UIViewAutoresizingFlexibleTopMargin |
                             UIViewAutoresizingFlexibleLeftMargin |
                             UIViewAutoresizingFlexibleRightMargin;
        [self.view addSubview:t];
        [UIView animateWithDuration:0.3 delay:3.0 options:0 animations:^{
            t.alpha = 0;
        } completion:^(BOOL fin) { [t removeFromSuperview]; }];
    });
}

#pragma mark - playback screen

- (void)showPlaybackScreen {
    [_loadingLabel removeFromSuperview];
    _loadingLabel = nil;

    _glView = [[APFAGLView alloc] initWithFrame:self.view.bounds];
    _glView.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                               UIViewAutoresizingFlexibleHeight;
    _glView.resizeDelegate = self;
    [self.view addSubview:_glView];

    // Hold (400 ms): pause/resume the moment the threshold is reached.
    // Double-tap (two short taps): hide/show the transport bar.
    UILongPressGestureRecognizer *hold =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self
                                                      action:@selector(onHold:)];
    hold.minimumPressDuration = 0.4;
    [_glView addGestureRecognizer:hold];

    UITapGestureRecognizer *dbl =
        [[UITapGestureRecognizer alloc] initWithTarget:self
                                                action:@selector(onDoubleTap:)];
    dbl.numberOfTapsRequired = 2;
    [_glView addGestureRecognizer:dbl];

    [self buildTransportBar];

    _startingOverlay = [[UILabel alloc] initWithFrame:self.view.bounds];
    _startingOverlay.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                                        UIViewAutoresizingFlexibleHeight;
    _startingOverlay.backgroundColor = [UIColor colorWithRed:18/255.0 green:18/255.0
                                                       blue:24/255.0 alpha:1.0];
    _startingOverlay.textColor     = [UIColor whiteColor];
    _startingOverlay.font          = [UIFont systemFontOfSize:16];
    _startingOverlay.textAlignment = NSTextAlignmentCenter;
    _startingOverlay.numberOfLines = 0;
    _startingOverlay.text = [NSString stringWithFormat:@"Starting...\n%@", _infoLine];
    [self.view addSubview:_startingOverlay];

    // Poll the seek slider at display rate. UI only, not timing-critical.
    _seekLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(pollSeek)];
    [_seekLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

    // The engine gets its surface from -glView:didResizeToPixelWidth:height:,
    // which UIKit calls as soon as the view is laid out — the twin of waiting
    // for surfaceCreated rather than starting blind here.
    [self.view setNeedsLayout];
}

- (void)buildTransportBar {
    CGFloat barH = 44;
    _bar = [[UIView alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width,
                                                    barH + [self topInset])];
    _bar.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    _bar.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];

    CGFloat y    = [self topInset];
    CGFloat left = [self sideInset] + 12;

    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(left, y, 60, barH)];
    title.text      = @"aPFA";
    title.textColor = [UIColor whiteColor];
    title.font      = [UIFont boldSystemFontOfSize:18];
    [_bar addSubview:title];

    CGFloat pauseW = 54;
    CGFloat rightEdge = self.view.bounds.size.width - [self sideInset] - 12;
    CGFloat sliderX = CGRectGetMaxX(title.frame) + 16;

    _seekBar = [[UISlider alloc] initWithFrame:
                CGRectMake(sliderX, y, rightEdge - pauseW - 8 - sliderX, barH)];
    _seekBar.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    _seekBar.minimumValue = 0;
    _seekBar.maximumValue = 1000;
    [_seekBar addTarget:self action:@selector(seekTouchDown)
       forControlEvents:UIControlEventTouchDown];
    [_seekBar addTarget:self action:@selector(seekTouchUp)
       forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                        UIControlEventTouchCancel];
    [_bar addSubview:_seekBar];

    _pauseButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _pauseButton.frame = CGRectMake(rightEdge - pauseW, y, pauseW, barH);
    _pauseButton.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin;
    _pauseButton.titleLabel.font = [UIFont systemFontOfSize:20];
    [_pauseButton setTitle:@"❚❚" forState:UIControlStateNormal];
    [_pauseButton setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    [_pauseButton addTarget:self action:@selector(togglePause)
           forControlEvents:UIControlEventTouchUpInside];
    [_bar addSubview:_pauseButton];

    [self.view addSubview:_bar];
}

- (CGFloat)topInset {
    if (@available(iOS 11.0, *)) return self.view.safeAreaInsets.top;
    return 0;
}

- (CGFloat)sideInset {
    if (@available(iOS 11.0, *)) return self.view.safeAreaInsets.left;
    return 0;
}

// The bar is laid out by hand rather than with constraints because it has to be
// re-placed on every rotation anyway: safeAreaInsets are not yet meaningful when
// buildTransportBar runs, and on a notched device they swap sides between
// landscape-left and landscape-right. Autoresizing masks alone would leave the
// slider under the notch in one of the two orientations.
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!_bar) return;

    CGFloat barH  = 44;
    CGFloat top   = [self topInset];
    CGFloat side  = [self sideInset];
    CGFloat width = self.view.bounds.size.width;

    _bar.frame = CGRectMake(0, 0, width, barH + top);

    CGFloat left      = side + 12;
    CGFloat rightEdge = width - side - 12;
    CGFloat pauseW    = 54;
    CGFloat titleW    = 60;

    for (UIView *v in _bar.subviews) {
        if ([v isKindOfClass:[UILabel class]]) v.frame = CGRectMake(left, top, titleW, barH);
    }
    CGFloat sliderX = left + titleW + 16;
    _seekBar.frame     = CGRectMake(sliderX, top,
                                    MAX(40, rightEdge - pauseW - 8 - sliderX), barH);
    _pauseButton.frame = CGRectMake(rightEdge - pauseW, top, pauseW, barH);
}

#pragma mark - seek

- (void)seekTouchDown { _userSeeking = YES; }

- (void)seekTouchUp {
    // Map the slider across [min, max] = PFA's [GetMinTime, GetMaxTime], so the
    // far left seeks into the -3s pre-roll (shows "-00:03") like stock PFA:
    // JumpTo(llFirstTime + (llLastTime-llFirstTime)*p/1000).
    int64_t minU = apfaGetMinMicros();
    int64_t maxU = apfaGetMaxMicros();
    if (maxU > minU)
        apfaSeek(minU + (maxU - minU) * (int64_t)_seekBar.value / 1000);
    _userSeeking = NO;
}

- (void)pollSeek {
    int64_t t    = apfaGetTimeMicros();
    int64_t minU = apfaGetMinMicros();
    int64_t maxU = apfaGetMaxMicros();
    if (!_userSeeking && maxU > minU)
        _seekBar.value = (float)(((t - minU) * 1000) / (maxU - minU));

    if (_startingOverlay && !_startingOverlay.hidden) {
        if (apfaIsPlaying()) {
            _startingOverlay.hidden = YES;
        } else {
            // Engine aborted during start-up (synth or GL init). Show why
            // instead of an infinite "Starting…" and stop polling — the user can
            // back out. The full driver error goes to the Xcode device console
            // (prefix [aPFA]).
            int err = apfaGetStartError();
            if (err != 0) {
                NSString *m;
                if (err == 1) {
                    m = @"Audio engine failed to start.\n\n"
                         "This device's audio output could not be opened.";
                } else if (err == 2) {
                    m = @"Graphics failed to initialize.\n\n"
                         "The OpenGL ES context could not be created. See the "
                         "device console (prefix [aPFA]) for details.";
                } else {
                    m = @"Playback failed to start.";
                }
                _startingOverlay.text = m;
                [_seekLink invalidate];
                _seekLink = nil;
            }
        }
    }
}

#pragma mark - gestures

- (void)onHold:(UILongPressGestureRecognizer *)g {
    if (g.state == UIGestureRecognizerStateBegan) [self togglePause];
}

- (void)onDoubleTap:(UITapGestureRecognizer *)g {
    _uiHidden = !_uiHidden;
    [UIView animateWithDuration:0.2 animations:^{
        self->_bar.alpha = self->_uiHidden ? 0 : 1;
    }];
}

- (void)togglePause {
    _paused = !_paused;
    if (_paused) apfaPause(); else apfaResume();
    [_pauseButton setTitle:(_paused ? @"▶" : @"❚❚") forState:UIControlStateNormal];
}

#pragma mark - APFAGLViewDelegate

- (void)glView:(APFAGLView *)view didResizeToPixelWidth:(int)w height:(int)h {
    if (!_started) {
        // First layout = the twin of surfaceCreated. Publish the size BEFORE
        // starting so the engine's first frame already has the right viewport
        // rather than rendering one frame at 0x0.
        apfaSurfaceChanged(w, h);
        apfaStart([view eaglLayerPtr]);
        _started = YES;
    } else {
        apfaSurfaceChanged(w, h);
    }
}

@end
