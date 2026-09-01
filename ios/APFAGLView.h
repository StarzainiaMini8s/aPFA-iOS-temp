// APFAGLView.h — the CAEAGLLayer-backed view the engine renders into.
//
// The Android twin is the SurfaceView in PlaybackActivity.showPlaybackScreen().
// This view does NOT drive rendering: it owns no display link and draws nothing.
// The engine's own thread owns the GL context and runs the clock, dispatch and
// draw back-to-back, exactly as PFA's GameThread does — this class exists only
// to hand that thread a drawable and to report size changes.
#import <UIKit/UIKit.h>

@class APFAGLView;

@protocol APFAGLViewDelegate <NSObject>
// Called on the MAIN thread whenever the drawable's pixel size changes
// (first layout, rotation). The twin of SurfaceHolder.Callback.surfaceChanged.
- (void)glView:(APFAGLView *)view didResizeToPixelWidth:(int)w height:(int)h;
@end

@interface APFAGLView : UIView

@property (nonatomic, weak) id<APFAGLViewDelegate> resizeDelegate;

// The CAEAGLLayer to hand to apfaStart(). Main thread only — the engine thread
// receives it as an opaque void* and never touches CALayer properties.
- (void *)eaglLayerPtr;

// Drawable size in PIXELS (points * contentsScale), not points.
- (int)pixelWidth;
- (int)pixelHeight;

@end
