// APFAPlaybackViewController.h — loading screen -> GL playback surface.
// The iOS twin of PlaybackActivity.
#import <UIKit/UIKit.h>

@interface APFAPlaybackViewController : UIViewController

// Paths are plain filesystem paths (already copied into the app container by
// the setup screen — the twin of PlaybackActivity.copyToCache). soundfontPath
// may be nil or empty for no soundfont.
- (instancetype)initWithMidiPath:(NSString *)midiPath
                   soundfontPath:(NSString *)soundfontPath
                      voiceCount:(int)voiceCount
                       noteSpeed:(float)noteSpeed
                         bgColor:(uint32_t)bgrColor
                    bgImagePath:(NSString *)bgImagePath;

@end
