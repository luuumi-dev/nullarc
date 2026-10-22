// A window that circles over the desktop 120 times a second, as a dragged window does, for WindowServer frame-rate
// measurements of the Tahoe materials: WT_MODE=plain (no materials), sidebar (a glass sidebar, as in Phone or Finder),
// toolbar (a glass toolbar over scrolled content, as in Safari), behind (the whole window blurs what is behind it),
// heavy (a sidebar, a glass toolbar and an inspector at once: about as many render passes a frame as a Safari window).
// Run in the logged-in GUI session: launchctl asuser <uid> ./wintest <seconds> (window 1200 x 800 points)
#import <Cocoa/Cocoa.h>

@interface Stripes : NSView
@end
@implementation Stripes
- (void)drawRect:(NSRect)r {
    for (int y = 0; y * 40 < self.bounds.size.height; y++) for (int x = 0; x * 40 < self.bounds.size.width; x++) {
        [[NSColor colorWithHue:((x + y) % 12) / 12.0 saturation:0.6 brightness:0.95 alpha:1] setFill];
        NSRectFill(NSMakeRect(x * 40, y * 40, 40, 40));
    }
}
@end

@interface Pane : NSViewController
@end
@implementation Pane
- (void)loadView { self.view = [[Stripes alloc] initWithFrame:NSMakeRect(0, 0, 600, 800)]; }
@end

@interface Side : NSViewController
@end
@implementation Side
- (void)loadView {
    NSStackView *s = [NSStackView stackViewWithViews:@[]];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    for (NSString *t in @[@"Recents", @"Favourites", @"Contacts", @"Voicemail", @"Calls"]) [s addArrangedSubview:[NSTextField labelWithString:t]];
    self.view = s;
}
@end

@interface TB : NSObject <NSToolbarDelegate>
@end
@implementation TB
- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)t { return @[@"back", @"fwd", NSToolbarFlexibleSpaceItemIdentifier, @"share"]; }
- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)t { return [self toolbarDefaultItemIdentifiers:t]; }
- (NSToolbarItem *)toolbar:(NSToolbar *)t itemForItemIdentifier:(NSToolbarItemIdentifier)i willBeInsertedIntoToolbar:(BOOL)f {
    NSToolbarItem *it = [[NSToolbarItem alloc] initWithItemIdentifier:i];
    NSString *sym = [i isEqual:@"back"] ? @"chevron.left" : [i isEqual:@"fwd"] ? @"chevron.right" : @"square.and.arrow.up";
    it.image = [NSImage imageWithSystemSymbolName:sym accessibilityDescription:i];
    it.label = i;
    return it;
}
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        const double secs = argc > 1 ? atof(argv[1]) : 15;
        const char *m = getenv("WT_MODE") ?: "plain";
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(500, 200, 1200, 800)
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                                                            NSWindowStyleMaskFullSizeContentView
                                                    backing:NSBackingStoreBuffered defer:NO];
        w.title = [NSString stringWithFormat:@"wintest %s", m];
        static TB *tb;
        if (!strcmp(m, "heavy")) {
            NSSplitViewController *sp = [NSSplitViewController new];
            [sp addSplitViewItem:[NSSplitViewItem sidebarWithViewController:[Side new]]];
            NSViewController *mid = [NSViewController new];
            NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 700, 800)];
            sv.documentView = [[Stripes alloc] initWithFrame:NSMakeRect(0, 0, 700, 3000)];
            mid.view = sv;
            [sp addSplitViewItem:[NSSplitViewItem contentListWithViewController:mid]];
            [sp addSplitViewItem:[NSSplitViewItem inspectorWithViewController:[Side new]]];
            w.contentViewController = sp;
            [w setContentSize:NSMakeSize(1200, 800)];
            tb = [TB new];
            NSToolbar *t = [[NSToolbar alloc] initWithIdentifier:@"wintest"];
            t.delegate = tb; w.toolbar = t; w.toolbarStyle = NSWindowToolbarStyleUnified;
        } else if (!strcmp(m, "sidebar")) {
            NSSplitViewController *sp = [NSSplitViewController new];
            [sp addSplitViewItem:[NSSplitViewItem sidebarWithViewController:[Side new]]];
            [sp addSplitViewItem:[NSSplitViewItem splitViewItemWithViewController:[Pane new]]];
            w.contentViewController = sp;
            [w setContentSize:NSMakeSize(1200, 800)];
        } else if (!strcmp(m, "toolbar")) {
            NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 800)];
            sv.documentView = [[Stripes alloc] initWithFrame:NSMakeRect(0, 0, 1200, 3000)];
            sv.hasVerticalScroller = YES;
            w.contentView = sv;
            tb = [TB new];
            NSToolbar *t = [[NSToolbar alloc] initWithIdentifier:@"wintest"];
            t.delegate = tb; w.toolbar = t; w.toolbarStyle = NSWindowToolbarStyleUnified;
            w.titlebarAppearsTransparent = YES;
        } else if (!strcmp(m, "behind")) {
            NSVisualEffectView *fx = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 800)];
            fx.material = NSVisualEffectMaterialSidebar; fx.blendingMode = NSVisualEffectBlendingModeBehindWindow;
            fx.state = NSVisualEffectStateActive;
            w.contentView = fx; w.opaque = NO; w.backgroundColor = NSColor.clearColor;
        } else {
            w.contentView = [[Stripes alloc] initWithFrame:NSMakeRect(0, 0, 1200, 800)];
        }
        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        __block double t = 0;
        [NSTimer scheduledTimerWithTimeInterval:1.0 / 120 repeats:YES block:^(NSTimer *tm) {
            t += 1.0 / 120;
            [w setFrameOrigin:NSMakePoint(500 + 250 * cos(t * 2), 200 + 150 * sin(t * 2))];
        }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        [NSApp run];
    }
    return 0;
}
