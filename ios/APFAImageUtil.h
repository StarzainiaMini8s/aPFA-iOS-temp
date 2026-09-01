// APFAImageUtil.h — decode a background image to the tightly packed RGBA the
// engine wants. The twin of PlaybackActivity.applyBgImage's
// BitmapFactory + getPixels + nativeSetBgImage path, minus the ARGB_8888
// unpacking: CGBitmapContext can be asked for the engine's byte order directly.
#import <Foundation/Foundation.h>

@interface APFAImageUtil : NSObject

// Decodes the image at `path`, scaled down so neither side exceeds `maxDim`
// (Android caps the same way at BG_MAX_DIM = 1280 — the image is stretched to
// the whole field anyway, so detail loss is invisible and a budget GPU gets a
// texture it can actually hold), then invokes `block` with tightly packed
// w*h*4 RGBA bytes, row 0 = top.
//
// The buffer is valid only for the duration of the block; the engine copies it.
// The block is NOT called if the image cannot be decoded.
+ (void)withRGBAOfImageAtPath:(NSString *)path
                       maxDim:(int)maxDim
                        block:(void (^)(const uint8_t *rgba, int w, int h))block;

@end
