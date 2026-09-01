// APFASetupViewController.h — aPFA setup screen, the iOS twin of MainActivity.
//
// Pure UI shell: picks the MIDI + soundfont and captures Voice Count and Note
// Speed. Settings and the chosen soundfont persist across launches in
// NSUserDefaults, under the SAME key names MainActivity uses in
// SharedPreferences, so a profile exported from one shell reads on the other.
#import <UIKit/UIKit.h>

@interface APFASetupViewController : UIViewController
@end
