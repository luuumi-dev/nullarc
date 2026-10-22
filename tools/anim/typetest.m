// Types one character every 0.4 s, as a keyboard does (insertText: through the field editor path), for checking that
// typed text stays on screen: the window's text pixels on the panel must only grow (tools/iosurf/scanout_watch).
//   plain  an NSTextView, black 72 pt text on white (the default)
//   glass  the same text view, transparent, over a glass panel (NSGlassEffectView, or NSVisualEffectView before 26)
//   field  a single-line NSTextField on a borderless glass panel, as Spotlight's search field
// Window content 1200 x 200 points at (200, 300). Prints "t <seconds> n <chars>" per keystroke.
// Run in the logged-in GUI session: launchctl asuser <uid> ./typetest [chars] [plain|glass|field]
#import <Cocoa/Cocoa.h>
#include <time.h>

static NSView *glass_panel(NSRect fr)
{
    Class g = NSClassFromString(@"NSGlassEffectView");
    if (g) { NSView *v = [[g alloc] initWithFrame:fr]; return v; }
    NSVisualEffectView *v = [[NSVisualEffectView alloc] initWithFrame:fr];
    v.material = NSVisualEffectMaterialHUDWindow; v.blendingMode = NSVisualEffectBlendingModeBehindWindow; v.state = NSVisualEffectStateActive;
    return v;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const int total = argc > 1 ? atoi(argv[1]) : 24;
        NSString *mode = argc > 2 ? @(argv[2]) : @"plain";
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        const NSRect fr = NSMakeRect(0, 0, 1200, 200);
        const BOOL field = [mode isEqualToString:@"field"], glass = field || [mode isEqualToString:@"glass"];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 300, 1200, 200)
                                                  styleMask:field ? NSWindowStyleMaskBorderless : NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered defer:NO];
        w.title = @"typetest";
        NSResponder *target;
        NSView *text;
        if (field) {
            w.opaque = NO; w.backgroundColor = NSColor.clearColor; w.hasShadow = YES; w.level = NSFloatingWindowLevel;
            NSTextField *tf = [[NSTextField alloc] initWithFrame:NSInsetRect(fr, 24, 50)];
            tf.font = [NSFont systemFontOfSize:72]; tf.bezeled = NO; tf.drawsBackground = NO; tf.focusRingType = NSFocusRingTypeNone;
            tf.textColor = NSColor.labelColor;
            text = tf; target = tf;
        } else {
            NSTextView *tv = [[NSTextView alloc] initWithFrame:fr];
            tv.font = [NSFont systemFontOfSize:72];
            tv.textColor = glass ? NSColor.labelColor : NSColor.blackColor;
            tv.backgroundColor = glass ? NSColor.clearColor : NSColor.whiteColor;
            tv.drawsBackground = !glass;
            text = tv; target = tv;
        }
        if (glass) {
            NSView *panel = glass_panel(fr);
            if ([panel respondsToSelector:@selector(setContentView:)]) [(id)panel setContentView:text];
            else [panel addSubview:text];
            w.contentView = panel;
        } else w.contentView = text;
        [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; [w makeFirstResponder:target];
        __block int n = 0; const double t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        NSString *src = @"Hello nullarc 0123456789 typing";
        [NSTimer scheduledTimerWithTimeInterval:0.4 repeats:YES block:^(NSTimer *tm) {
            if (n >= total) { [NSApp terminate:nil]; return; }
            NSString *ch = [src substringWithRange:NSMakeRange(n % src.length, 1)];
            if (field) [(NSTextView *)[w fieldEditor:YES forObject:target] insertText:ch replacementRange:NSMakeRange(NSNotFound, 0)];
            else [(NSTextView *)target insertText:ch replacementRange:NSMakeRange(NSNotFound, 0)];
            n++;
            printf("t %.3f n %d\n", clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9 - t0, n); fflush(stdout);
        }];
        [NSApp run];
    }
    return 0;
}
