// APFASetupViewController.m — see the header.
//
// Knobs MainActivity has that are deliberately absent here, all for the same
// reason (the thing they control does not exist in the iOS target):
//   • Core Affinity      — no sched_setaffinity on iOS; the engine thread takes
//                          QOS_CLASS_USER_INTERACTIVE instead (engine.cpp).
//   • Legacy Renderer    — iOS always gets RendererES2, which asks for an ES3
//                          context by itself and falls back on its own.
//   • Chunked Disk       — the streaming pool is not compiled in (no
//     Streaming /          APFA_STREAMING), so every load is the in-RAM parse.
//     Pagefile on SD       iOS has no removable storage and no swap either way.
#import "APFASetupViewController.h"
#import "APFAPlaybackViewController.h"

#import <MobileCoreServices/MobileCoreServices.h>
#include <math.h>

// Pref keys — identical to MainActivity's SharedPreferences keys.
static NSString *const kPrefVoiceCount = @"voiceCount";
static NSString *const kPrefNoteSpeed  = @"noteSpeed";
static NSString *const kPrefBgColor    = @"bgColor";
static NSString *const kPrefBgImage    = @"bgImage";
static NSString *const kPrefSoundfont  = @"soundfont";

typedef NS_ENUM(NSInteger, APFAPickTarget) {
    APFAPickMidi,
    APFAPickSoundfont,
};

@interface APFASetupViewController ()
    <UIDocumentPickerDelegate, UINavigationControllerDelegate,
     UIImagePickerControllerDelegate>
@end

@implementation APFASetupViewController {
    NSString *_midiPath;        // in the app container, ready for the engine
    NSString *_midiName;
    NSString *_sfPath;
    NSString *_sfName;
    int       _voiceCount;
    float     _noteSpeed;
    uint32_t  _bgColor;         // PFA BGR packing: R = bits 0-7, G = 8-15, B = 16-23
    NSString *_bgImagePath;

    APFAPickTarget _pickTarget;

    UIScrollView *_scroll;
    UIButton *_midiButton;
    UIButton *_sfButton;
    UILabel  *_voiceLabel;
    UISlider *_voiceSlider;
    UILabel  *_speedLabel;
    UISlider *_speedSlider;
    UIView   *_colorSwatch;
    UISlider *_rSlider, *_gSlider, *_bSlider;
    UIButton *_playButton;
}

#pragma mark - lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"aPFA";
    self.view.backgroundColor = [UIColor colorWithRed:18/255.0 green:18/255.0
                                                 blue:24/255.0 alpha:1.0];
    _voiceCount = 250;
    _noteSpeed  = 0.05f;
    _bgColor    = 0x00464646;   // PFA's default
    [self loadSettings];
    [self buildSetupScreen];
    [self refreshLabels];
}

- (BOOL)prefersStatusBarHidden { return NO; }

#pragma mark - settings persistence

- (void)loadSettings {
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    if ([d objectForKey:kPrefVoiceCount]) _voiceCount = (int)[d integerForKey:kPrefVoiceCount];
    if ([d objectForKey:kPrefNoteSpeed])  _noteSpeed  = [d floatForKey:kPrefNoteSpeed];
    if ([d objectForKey:kPrefBgColor])    _bgColor    = (uint32_t)[d integerForKey:kPrefBgColor];

    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *bg = [d stringForKey:kPrefBgImage];
    _bgImagePath = (bg.length && [fm fileExistsAtPath:bg]) ? bg : nil;

    // The soundfont is remembered across launches, like Android's. A stale path
    // (app container re-signed / reinstalled) is dropped rather than handed to
    // the engine as a file that is not there.
    NSString *sf = [d stringForKey:kPrefSoundfont];
    if (sf.length && [fm fileExistsAtPath:sf]) {
        _sfPath = sf;
        _sfName = sf.lastPathComponent;
    }
}

- (void)saveSettings {
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    [d setInteger:_voiceCount forKey:kPrefVoiceCount];
    [d setFloat:_noteSpeed    forKey:kPrefNoteSpeed];
    [d setInteger:(NSInteger)_bgColor forKey:kPrefBgColor];
    [d setObject:(_bgImagePath ?: @"") forKey:kPrefBgImage];
    [d setObject:(_sfPath ?: @"")      forKey:kPrefSoundfont];
    [d synchronize];
}

#pragma mark - note speed mapping (identical to MainActivity)

// Non-linear, 0.05 at the midpoint — the whole lower half of the slider covers
// 0.005..0.05, which is where every usable speed actually lives.
static float speedFromProgress(int p) {
    return (p <= 500) ? 0.005f + (p / 500.0f) * 0.045f
                      : 0.05f  + ((p - 500) / 500.0f) * 0.95f;
}

static int progressFromSpeed(float s) {
    if (s <= 0.05f) {
        int v = (int)(((s - 0.005f) / 0.045f) * 500.0f);
        return MAX(0, MIN(500, v));
    }
    int v = (int)(500 + ((s - 0.05f) / 0.95f) * 500.0f);
    return MAX(500, MIN(1000, v));
}

#pragma mark - UI

- (void)buildSetupScreen {
    _scroll = [[UIScrollView alloc] initWithFrame:self.view.bounds];
    _scroll.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                               UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_scroll];

    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis         = UILayoutConstraintAxisVertical;
    stack.spacing      = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_scroll addSubview:stack];

    UILayoutGuide *guide = self.view.layoutMarginsGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor      constraintEqualToAnchor:_scroll.topAnchor constant:24],
        [stack.bottomAnchor   constraintEqualToAnchor:_scroll.bottomAnchor constant:-24],
        [stack.leadingAnchor  constraintEqualToAnchor:guide.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor],
    ]];

    _midiButton = [self buttonTitled:@"Choose MIDI..." action:@selector(pickMidi)];
    [stack addArrangedSubview:_midiButton];

    _sfButton = [self buttonTitled:@"Choose Soundfont..." action:@selector(pickSoundfont)];
    [stack addArrangedSubview:_sfButton];

    _voiceLabel  = [self label:@"Voice Count"];
    [stack addArrangedSubview:_voiceLabel];
    _voiceSlider = [[UISlider alloc] init];
    _voiceSlider.minimumValue = 1;
    _voiceSlider.maximumValue = 500;
    _voiceSlider.value        = _voiceCount;
    [_voiceSlider addTarget:self action:@selector(voiceChanged)
           forControlEvents:UIControlEventValueChanged];
    [stack addArrangedSubview:_voiceSlider];

    _speedLabel  = [self label:@"Note Speed"];
    [stack addArrangedSubview:_speedLabel];
    _speedSlider = [[UISlider alloc] init];
    _speedSlider.minimumValue = 0;
    _speedSlider.maximumValue = 1000;
    _speedSlider.value        = progressFromSpeed(_noteSpeed);
    [_speedSlider addTarget:self action:@selector(speedChanged)
           forControlEvents:UIControlEventValueChanged];
    [stack addArrangedSubview:_speedSlider];

    [stack addArrangedSubview:[self label:@"Background"]];
    _colorSwatch = [[UIView alloc] init];
    _colorSwatch.layer.borderColor = [UIColor whiteColor].CGColor;
    _colorSwatch.layer.borderWidth = 1;
    [_colorSwatch.heightAnchor constraintEqualToConstant:28].active = YES;
    [stack addArrangedSubview:_colorSwatch];

    _rSlider = [self colorSlider];
    _gSlider = [self colorSlider];
    _bSlider = [self colorSlider];
    // PFA stores the background as BGR, not RGB — red is the LOW byte. Unpack it
    // the same way here so a colour set on Android looks the same on iOS.
    _rSlider.value = (_bgColor >>  0) & 0xFF;
    _gSlider.value = (_bgColor >>  8) & 0xFF;
    _bSlider.value = (_bgColor >> 16) & 0xFF;
    [stack addArrangedSubview:_rSlider];
    [stack addArrangedSubview:_gSlider];
    [stack addArrangedSubview:_bSlider];

    UIStackView *imgRow = [[UIStackView alloc] init];
    imgRow.axis = UILayoutConstraintAxisHorizontal;
    imgRow.spacing = 8;
    imgRow.distribution = UIStackViewDistributionFillEqually;
    [imgRow addArrangedSubview:[self buttonTitled:@"Background Image..."
                                           action:@selector(pickBgImage)]];
    [imgRow addArrangedSubview:[self buttonTitled:@"Clear Image"
                                           action:@selector(clearBgImage)]];
    [stack addArrangedSubview:imgRow];

    _playButton = [self buttonTitled:@"Play" action:@selector(play)];
    _playButton.titleLabel.font = [UIFont boldSystemFontOfSize:20];
    [_playButton.heightAnchor constraintEqualToConstant:52].active = YES;
    [stack addArrangedSubview:_playButton];

    [stack addArrangedSubview:[self label:
        @"Tip: MIDIs and soundfonts can also be dropped into aPFA's folder in "
         "the Files app (On My iPhone -> aPFA), or over USB in Finder."]];
}

- (UILabel *)label:(NSString *)text {
    UILabel *l = [[UILabel alloc] init];
    l.text          = text;
    l.textColor     = [UIColor whiteColor];
    l.font          = [UIFont systemFontOfSize:15];
    l.numberOfLines = 0;
    return l;
}

- (UIButton *)buttonTitled:(NSString *)title action:(SEL)sel {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    [b setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    b.titleLabel.font          = [UIFont systemFontOfSize:17];
    b.titleLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    b.backgroundColor          = [UIColor colorWithWhite:1 alpha:0.12];
    b.layer.cornerRadius       = 8;
    [b.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [b addTarget:self action:sel forControlEvents:UIControlEventTouchUpInside];
    return b;
}

- (UISlider *)colorSlider {
    UISlider *s = [[UISlider alloc] init];
    s.minimumValue = 0;
    s.maximumValue = 255;
    [s addTarget:self action:@selector(colorChanged)
        forControlEvents:UIControlEventValueChanged];
    return s;
}

- (void)refreshLabels {
    [_midiButton setTitle:(_midiName ? [@"MIDI: " stringByAppendingString:_midiName]
                                     : @"Choose MIDI...")
                 forState:UIControlStateNormal];
    [_sfButton setTitle:(_sfName ? [@"SF: " stringByAppendingString:_sfName]
                                 : @"Choose Soundfont...")
               forState:UIControlStateNormal];
    _voiceLabel.text = [NSString stringWithFormat:@"Voice Count: %d", _voiceCount];
    _speedLabel.text = [NSString stringWithFormat:@"Note Speed: %.3f", _noteSpeed];
    _colorSwatch.backgroundColor =
        [UIColor colorWithRed:((_bgColor >>  0) & 0xFF) / 255.0
                        green:((_bgColor >>  8) & 0xFF) / 255.0
                         blue:((_bgColor >> 16) & 0xFF) / 255.0
                        alpha:1.0];
    _playButton.enabled = (_midiPath != nil);
    _playButton.alpha   = _midiPath ? 1.0 : 0.4;
}

#pragma mark - control actions

- (void)voiceChanged {
    _voiceCount = MAX(1, (int)lround(_voiceSlider.value));
    [self refreshLabels];
}

- (void)speedChanged {
    _noteSpeed = speedFromProgress((int)lround(_speedSlider.value));
    [self refreshLabels];
}

- (void)colorChanged {
    uint32_t r = (uint32_t)lround(_rSlider.value);
    uint32_t g = (uint32_t)lround(_gSlider.value);
    uint32_t b = (uint32_t)lround(_bSlider.value);
    _bgColor = r | (g << 8) | (b << 16);   // PFA BGR
    // A solid colour and an image are alternatives, not layers — picking a
    // colour clears the image, exactly as MainActivity does.
    _bgImagePath = nil;
    [self refreshLabels];
}

#pragma mark - file picking

- (void)pickMidi {
    _pickTarget = APFAPickMidi;
    [self presentDocumentPicker];
}

- (void)pickSoundfont {
    _pickTarget = APFAPickSoundfont;
    [self presentDocumentPicker];
}

- (void)presentDocumentPicker {
    // Import mode, not Open: iOS COPIES the file into a temporary container
    // location we own outright, so there is no security-scoped bookmark to keep
    // alive and no sandbox question when the engine mmaps it later. .sf2/.sfz
    // and .mid have no useful shared UTI, so the filter is public.data — the
    // same "let the user pick anything" stance the Android SAF call takes.
    UIDocumentPickerViewController *p =
        [[UIDocumentPickerViewController alloc]
            initWithDocumentTypes:@[(NSString *)kUTTypeData, (NSString *)kUTTypeContent]
                           inMode:UIDocumentPickerModeImport];
    p.delegate = self;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject;
    if (!url) return;
    [self acceptPickedURL:url];
}

// iOS 8-10 delegate shape, kept so the app still behaves if it is ever built
// against an older SDK.
- (void)documentPicker:(UIDocumentPickerViewController *)controller
 didPickDocumentAtURL:(NSURL *)url {
    [self acceptPickedURL:url];
}

- (void)acceptPickedURL:(NSURL *)url {
    NSString *name = url.lastPathComponent ?: @"input";
    // Import mode already handed us a copy, but in the app's *temporary*
    // directory, which iOS may purge at any time — including between the pick
    // and the Play tap. Move it somewhere durable now.
    NSString *dest = [self containerPathForName:name
                                     subdirectory:(_pickTarget == APFAPickMidi
                                                   ? @"midi" : @"soundfonts")];
    NSError *err = nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:dest]) [fm removeItemAtPath:dest error:nil];
    if (![fm copyItemAtURL:url toURL:[NSURL fileURLWithPath:dest] error:&err]) {
        [self alert:@"Could not read the file"
            message:err.localizedDescription ?: @"Unknown error"];
        return;
    }

    if (_pickTarget == APFAPickMidi) {
        _midiPath = dest;
        _midiName = name;
    } else {
        _sfPath = dest;
        _sfName = name;
    }
    [self saveSettings];
    [self refreshLabels];
}

- (NSString *)containerPathForName:(NSString *)name
                      subdirectory:(NSString *)sub {
    NSString *docs = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *dir = [docs stringByAppendingPathComponent:sub];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                             withIntermediateDirectories:YES
                                              attributes:nil
                                                   error:nil];
    return [dir stringByAppendingPathComponent:name];
}

#pragma mark - background image

- (void)pickBgImage {
    UIImagePickerController *p = [[UIImagePickerController alloc] init];
    p.sourceType = UIImagePickerControllerSourceTypePhotoLibrary;
    p.delegate   = self;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)imagePickerController:(UIImagePickerController *)picker
didFinishPickingMediaWithInfo:(NSDictionary<UIImagePickerControllerInfoKey, id> *)info {
    [picker dismissViewControllerAnimated:YES completion:nil];
    UIImage *img = info[UIImagePickerControllerOriginalImage];
    if (!img) return;
    // Re-encode into the container rather than keeping an asset reference: the
    // playback screen decodes it from a plain path, on a background queue, and
    // must not need photo-library access at that point.
    NSData *png = UIImagePNGRepresentation(img);
    if (!png) return;
    NSString *dest = [self containerPathForName:@"background.png" subdirectory:@"bg"];
    if (![png writeToFile:dest atomically:YES]) return;
    _bgImagePath = dest;
    [self saveSettings];
    [self refreshLabels];
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController *)picker {
    [picker dismissViewControllerAnimated:YES completion:nil];
}

- (void)clearBgImage {
    _bgImagePath = nil;
    [self saveSettings];
    [self refreshLabels];
}

#pragma mark - play

- (void)play {
    if (!_midiPath) {
        [self alert:@"No MIDI selected" message:@"Choose a MIDI file first."];
        return;
    }
    [self saveSettings];
    APFAPlaybackViewController *vc =
        [[APFAPlaybackViewController alloc] initWithMidiPath:_midiPath
                                              soundfontPath:_sfPath
                                                 voiceCount:_voiceCount
                                                  noteSpeed:_noteSpeed
                                                    bgColor:_bgColor
                                                bgImagePath:_bgImagePath];
    [self.navigationController pushViewController:vc animated:YES];
}

- (void)alert:(NSString *)title message:(NSString *)msg {
    UIAlertController *a =
        [UIAlertController alertControllerWithTitle:title
                                            message:msg
                                     preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK"
                                          style:UIAlertActionStyleDefault
                                        handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

@end
