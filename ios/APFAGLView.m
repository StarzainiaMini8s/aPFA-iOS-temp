// APFAGLView.m — see APFAGLView.h.
#import "APFAGLView.h"
#import <QuartzCore/QuartzCore.h>
#import <OpenGLES/EAGLDrawable.h>
#include <math.h>

@implementation APFAGLView {
    int _lastW;
    int _lastH;
}

+ (Class)layerClass { return [CAEAGLLayer class]; }

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;

    // Every CALayer property the drawable depends on is configured HERE, on the
    // main thread, before Engine::start() ever hands the layer to the engine
    // thread. eagl_surface.mm deliberately reads none of them back.
    CAEAGLLayer *layer = (CAEAGLLayer *)self.layer;
    layer.opaque = YES;
    layer.drawableProperties = @{
        // NO: we redraw the whole field every frame, so retaining the previous
        // contents would only cost bandwidth.
        kEAGLDrawablePropertyRetainedBacking : @NO,
        kEAGLDrawablePropertyColorFormat     : kEAGLColorFormatRGBA8,
    };

    // Native resolution. On a 5S that is 640x1136 — the honest pixel count, so
    // the frame rate aPFA reports is the frame rate the GPU actually sustains
    // at the resolution the user is looking at.
    self.contentScaleFactor = [UIScreen mainScreen].scale;

    self.opaque = YES;
    self.backgroundColor = [UIColor blackColor];
    self.multipleTouchEnabled = NO;
    return self;
}

- (void *)eaglLayerPtr { return (__bridge void *)self.layer; }

- (int)pixelWidth {
    return (int)lround(self.bounds.size.width * self.layer.contentsScale);
}

- (int)pixelHeight {
    return (int)lround(self.bounds.size.height * self.layer.contentsScale);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    int w = [self pixelWidth];
    int h = [self pixelHeight];
    if (w <= 0 || h <= 0) return;
    if (w == _lastW && h == _lastH) return;
    _lastW = w;
    _lastH = h;
    [self.resizeDelegate glView:self didResizeToPixelWidth:w height:h];
}

@end
