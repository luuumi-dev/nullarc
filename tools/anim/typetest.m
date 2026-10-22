// Types into an NSTextView one character every 0.4 s, as a keyboard does (insertText: through the field editor path),
// for checking that typed text stays on screen: the window's text pixels on the panel must only grow.
// Window 1200 x 200 points at (200, 300), black 72 pt text on white. Prints "t <seconds> n <chars>" per keystroke.
// Run in the logged-in GUI session: launchctl asuser <uid> ./typetest [chars]
#import <Cocoa/Cocoa.h>
#include <time.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        const int total = argc > 1 ? atoi(argv[1]) : 24;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 300, 1200, 200)
                                                  styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        w.title = @"typetest";
        NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 200)];
        tv.font = [NSFont systemFontOfSize:72]; tv.textColor = NSColor.blackColor; tv.backgroundColor = NSColor.whiteColor;
        tv.drawsBackground = YES;
        w.contentView = tv;
        [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; [w makeFirstResponder:tv];
        __block int n = 0; const double t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        NSString *src = @"Hello nullarc 0123456789 typing";
        [NSTimer scheduledTimerWithTimeInterval:0.4 repeats:YES block:^(NSTimer *tm) {
            if (n >= total) { [NSApp terminate:nil]; return; }
            [tv insertText:[src substringWithRange:NSMakeRange(n % src.length, 1)] replacementRange:NSMakeRange(NSNotFound, 0)];
            n++;
            printf("t %.3f n %d\n", clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9 - t0, n); fflush(stdout);
        }];
        [NSApp run];
    }
    return 0;
}
