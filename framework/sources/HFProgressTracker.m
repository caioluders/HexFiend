//
//  HFProgressTracker.m
//  HexFiend_2
//
//  Created by peter on 2/12/08.
//  Copyright 2008 ridiculous_fish. All rights reserved.
//

#import <HexFiend/HFProgressTracker.h>
#import <HexFiend/HFAssert.h>
#if !defined(__APPLE__)
    #include <stdatomic.h>
#endif

@implementation HFProgressTracker

- (void)setMaxProgress:(unsigned long long)max {
    maxProgress = max;
}

- (unsigned long long)maxProgress {
    return maxProgress;
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
- (void)setProgressIndicator:(NSProgressIndicator *)indicator {
    progressIndicator = indicator;
}

- (NSProgressIndicator *)progressIndicator {
    return progressIndicator;
}
#endif

- (void)_updateProgress:(NSTimer *)timer {
    USE(timer);
    double value;
    unsigned long long localCurrentProgress = currentProgress;
    if (maxProgress == 0 || localCurrentProgress == 0) {
        value = 0;
    }
    else {
        value = (double)((long double)localCurrentProgress / (long double)maxProgress);
    }
    if (value != lastSetValue) {
        lastSetValue = value;
#if defined(__APPLE__) && !TARGET_OS_IPHONE
        [progressIndicator setDoubleValue:lastSetValue];
#endif
        if (delegate && [delegate respondsToSelector:@selector(progressTracker:didChangeProgressTo:)]) {
            [delegate progressTracker:self didChangeProgressTo:lastSetValue];
        }
    }
}

- (void)beginTrackingProgress {
    HFASSERT(progressTimer == NULL);
    NSRunLoop *currentRunLoop = [NSRunLoop currentRunLoop];
    progressTimer = [NSTimer timerWithTimeInterval:1 / 30. target:self selector:@selector(_updateProgress:) userInfo:nil repeats:YES];
    [currentRunLoop addTimer:progressTimer forMode:NSDefaultRunLoopMode];
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    [currentRunLoop addTimer:progressTimer forMode:NSModalPanelRunLoopMode];
#endif
    [self _updateProgress:nil];
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    [progressIndicator startAnimation:self];
#endif
}

- (void)endTrackingProgress {
    HFASSERT(progressTimer != NULL);
    [progressTimer invalidate];
    progressTimer = nil;
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    [progressIndicator stopAnimation:self];
#endif
}

- (void)requestCancel:(id)sender {
    USE(sender);
    cancelRequested = 1;
#if defined(__APPLE__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSMemoryBarrier();
    #pragma clang diagnostic pop
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

- (void)dealloc {
    [progressTimer invalidate];
    progressTimer = nil;
}

- (void)setDelegate:(id)val {
    delegate = val;
}

- (id)delegate {
    return delegate;
}

- (void)noteFinished:(id)sender {
    if (delegate != nil) {   
        if (!NSThread.isMainThread) {
            [self performSelectorOnMainThread:@selector(noteFinished:) withObject:sender waitUntilDone:NO];
        }
        else {
            if ([delegate respondsToSelector:@selector(progressTrackerDidFinish:)]) {
                [delegate progressTrackerDidFinish:self];
            }
        }
    }
}

@end
