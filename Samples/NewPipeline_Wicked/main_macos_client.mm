#include "NewPipelineClientApp.h"

#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>

wicked_newpipeline::NewPipelineClientApp application;
bool running = true;

@interface WindowDelegate : NSObject <NSWindowDelegate>
@end

int main(int argc, char* argv[])
{
    @autoreleasepool
    {
        wi::arguments::Parse(argc, argv);
        application.ConfigureFromCommandLine(argc, argv);

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        CGRect frame = (CGRect){ {100.0, 100.0}, {1280.0, 720.0} };
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:(NSWindowStyleMaskTitled |
                                                                  NSWindowStyleMaskClosable |
                                                                  NSWindowStyleMaskMiniaturizable |
                                                                  NSWindowStyleMaskResizable)
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        [window setTitle:[NSString stringWithUTF8String:application.GetWindowTitle().c_str()]];
        [window center];
        [window makeKeyAndOrderFront:nil];

        WindowDelegate* delegate = [[WindowDelegate alloc] init];
        [window setDelegate:delegate];

        [NSApp activateIgnoringOtherApps:YES];

        application.SetWindow((__bridge wi::platform::window_type)window);

        while (running)
        {
            @autoreleasepool
            {
                NSEvent* event;
                while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:[NSDate distantPast]
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES]))
                {
                    switch (event.type)
                    {
                    case NSEventTypeKeyDown:
                        switch (event.keyCode)
                        {
                        case kVK_Delete:
                            wi::gui::TextInputField::DeleteFromInput();
                            break;
                        case kVK_Return:
                            break;
                        default:
                        {
                            NSString* characters = event.characters;
                            if (characters && characters.length > 0)
                            {
                                unichar c = [characters characterAtIndex:0];
                                wchar_t wchar = (wchar_t)c;
                                wi::gui::TextInputField::AddInput(wchar);
                            }
                        }
                        break;
                        }
                        break;
                    case NSEventTypeKeyUp:
                        break;
                    default:
                        [NSApp sendEvent:event];
                        break;
                    }
                }

                application.Run();
            }
        }

        wi::jobsystem::ShutDown();
    }

    return 0;
}

@implementation WindowDelegate
- (void)windowWillClose:(NSNotification*)notification
{
    running = false;
}

- (void)windowDidResize:(NSNotification*)notification
{
    NSWindow* nsWindow = (NSWindow*)notification.object;
    application.SetWindow((__bridge wi::platform::window_type)nsWindow);
}
@end
