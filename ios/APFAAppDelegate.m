// APFAAppDelegate.m — see the header.
//
// Built without a storyboard on purpose: the window and the root controller are
// created in code, so the whole shell is readable as source and nothing depends
// on a nib the Linux cross-compile path cannot produce (see ios/README.md).
#import "APFAAppDelegate.h"
#import "APFASetupViewController.h"

@implementation APFAAppDelegate

- (BOOL)application:(UIApplication *)application
didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
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
