// APFAAppDelegate.m — see the header.
//
// Built without a storyboard on purpose: the window and the root controller are
// created in code, so the whole shell is readable as source and nothing depends
// on a nib the Linux cross-compile path cannot produce (see ios/README.md).
#import "APFAAppDelegate.h"
#import "APFASetupViewController.h"
#import "apfa_bridge.h"

@implementation APFAAppDelegate

- (BOOL)application:(UIApplication *)application
didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // First thing, before any engine call can log: put the shared engine's
    // LOGI/LOGE into a file the phone can show. A sideloaded build has no
    // console anyone here can read — stdout/stderr need a Mac with Xcode
    // attached — and a load that refuses is exactly when its reasoning is
    // needed. UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace (see
    // Info.plist) publish Documents as "On My iPhone -> aPFA", so this lands
    // beside the midi/ and soundfonts/ folders as apfa-log.txt.
    NSString *docs = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (docs) {
        apfaSetLogPath(
            [docs stringByAppendingPathComponent:@"apfa-log.txt"].UTF8String);
    }

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];

    APFASetupViewController *setup = [[APFASetupViewController alloc] init];
    UINavigationController *nav =
        [[UINavigationController alloc] initWithRootViewController:setup];
    nav.navigationBar.barStyle = UIBarStyleBlack;
    nav.navigationBar.tintColor = [UIColor whiteColor];
    nav.navigationBar.titleTextAttributes =
        @{ NSForegroundColorAttributeName : [UIColor whiteColor] };

    self.window.rootViewController = nav;
    self.window.backgroundColor    = [UIColor blackColor];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
