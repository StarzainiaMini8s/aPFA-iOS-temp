// APFAImageUtil.m — see the header.
#import "APFAImageUtil.h"
#import <UIKit/UIKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <math.h>

@implementation APFAImageUtil

+ (void)withRGBAOfImageAtPath:(NSString *)path
                       maxDim:(int)maxDim
                        block:(void (^)(const uint8_t *, int, int))block {
    if (!path.length || !block) return;

    UIImage *img = [UIImage imageWithContentsOfFile:path];
    CGImageRef cg = img.CGImage;
    if (!cg) return;

    int srcW = (int)CGImageGetWidth(cg);
    int srcH = (int)CGImageGetHeight(cg);
    if (srcW <= 0 || srcH <= 0) return;

    int w = srcW, h = srcH;
    if (maxDim > 0 && (w > maxDim || h > maxDim)) {
        double s = (double)maxDim / (double)MAX(w, h);
        w = MAX(1, (int)lround(w * s));
        h = MAX(1, (int)lround(h * s));
    }

    size_t stride = (size_t)w * 4;
    uint8_t *buf = (uint8_t *)calloc((size_t)h * stride, 1);
    if (!buf) return;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    // kCGImageAlphaPremultipliedLast + default (big-endian) byte order gives
    // R,G,B,A in ascending memory order — exactly the layout the engine's
    // uploadBgImage expects, so there is nothing to shuffle afterwards.
    CGContextRef ctx = CGBitmapContextCreate(buf, (size_t)w, (size_t)h, 8, stride, cs,
                                             kCGImageAlphaPremultipliedLast |
                                             kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(buf); return; }

    // CoreGraphics draws with the origin at the BOTTOM left, so a straight draw
    // would hand the engine a vertically mirrored image. Flip the CTM so row 0
    // of the buffer is the TOP row of the picture, which is what the header
    // promises and what the shader assumes.
    CGContextTranslateCTM(ctx, 0, h);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);

    block(buf, w, h);
    free(buf);
}

@end
