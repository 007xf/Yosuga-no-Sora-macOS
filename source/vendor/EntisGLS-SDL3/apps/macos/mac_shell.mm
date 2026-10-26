#include "apps/macos/mac_shell.h"
#include "apps/macos/guide_overlay.h"
#include "runtime/cotopha_port/host_bridge.h"
#include "runtime/cotopha_port/game_progress.h"
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

using entis::host::Bridge;
using entis::host::Event;
using entis::host::Game;
using entis::host::Request;

@interface EntisMacShellController : NSObject <NSWindowDelegate>
- (void)performShellAction:(id)sender;
- (void)startupLogoChanged:(id)sender;
- (void)displayModeChanged:(id)sender;
- (void)settingsTabChanged:(id)sender;
@end

namespace entis::mac {
namespace {
NSWindow* window = nil;
NSWindow* settingsWindow = nil;
NSWindow* snapshotPanel = nil;
NSWindow* aboutWindow = nil;
NSTabView* settingsTabs = nil;
NSSegmentedControl* settingsTabControl = nil;
NSString* dataRoot = nil;
NSSwitch* startupSwitch = nil;
NSSegmentedControl* displayMode = nil;
EntisMacShellController* controller = nil;
id keyMonitor = nil;
std::atomic<bool> clearDataPending{false};
NSRect testWindowFrame = NSZeroRect;
bool hasTestWindowFrame = false;
NSString* const startupLogoKey = @"YosugaStartupLogoEnabled";

NSWindow* WindowOf(SDL_Window* sdlWindow) {
    return (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
                                                      SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
}

void OnMain(dispatch_block_t block) {
    if (NSThread.isMainThread) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

NSTextField* Label(NSString* text, NSRect frame, bool heading = false) {
    NSTextField* label = [NSTextField wrappingLabelWithString:text ?: @""];
    label.frame = frame;
    label.font = heading ? [NSFont boldSystemFontOfSize:13] : [NSFont systemFontOfSize:13];
    label.textColor = NSColor.labelColor;
    return label;
}

NSButton* Button(NSString* title, NSString* command, NSRect frame) {
    NSButton* button = [NSButton buttonWithTitle:title target:controller action:@selector(performShellAction:)];
    button.frame = frame;
    button.bezelStyle = NSBezelStyleRounded;
    button.identifier = command;
    return button;
}

NSBox* Group(NSString* title, NSRect frame, NSView* parent) {
    NSBox* box = [[NSBox alloc] initWithFrame:frame];
    box.title = title;
    [parent addSubview:box];
    return box;
}

void PostGameCommand(NSString* command) {
    if (guide::Visible()) return;  // the in-game route map owns input while open
    Request request;
    request.kind = Request::Kind::GameCommand;
    request.game = Bridge::Get().ActiveGame();
    const std::string utf8 = command.UTF8String ?: "";
    request.command.assign(utf8.begin(), utf8.end());  // command IDs are ASCII
    Bridge::Get().Post(request);
}

void RequestQuit() {
    Request request;
    request.kind = Request::Kind::Quit;
    Bridge::Get().Post(request);
}

void SyncDisplayMode() {
    if (displayMode) displayMode.selectedSegment = (window.styleMask & NSWindowStyleMaskFullScreen) ? 1 : 0;
}

void SetFullscreen(bool enabled) {
    if (window && enabled != ((window.styleMask & NSWindowStyleMaskFullScreen) != 0))
        [window toggleFullScreen:nil];
    SyncDisplayMode();
}

void TestWindowAspect(bool wide) {
    const char* driver = SDL_getenv("ENTISGLS_DRIVER");
    const char* saves = SDL_getenv("ENTISGLS_TEST_SAVEDIR");
    if (!driver || !*driver || !saves || !*saves || !window ||
        (window.styleMask & NSWindowStyleMaskFullScreen)) return;
    if (!wide) {
        if (!hasTestWindowFrame) return;
        window.contentAspectRatio = NSMakeSize(4, 3);
        [window setFrame:testWindowFrame display:YES];
        hasTestWindowFrame = false;
        return;
    }
    if (!hasTestWindowFrame) testWindowFrame = window.frame;
    hasTestWindowFrame = true;
    const NSRect visible = (window.screen ?: NSScreen.mainScreen).visibleFrame;
    const CGFloat titleBar = NSHeight(window.frame) - NSHeight(window.contentLayoutRect);
    const CGFloat width = std::floor(std::min<CGFloat>(960, std::min(NSWidth(visible) * 0.95,
        (NSHeight(visible) - titleBar) * 0.92 * 16 / 9)));
    window.contentAspectRatio = NSMakeSize(16, 9);
    [window setContentSize:NSMakeSize(width, std::floor(width * 9 / 16))];
}

NSWindow* Panel(NSString* title, NSSize size) {
    NSWindow* panel = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered defer:NO];
    panel.title = title;
    panel.releasedWhenClosed = NO;
    panel.delegate = controller;
    [panel center];
    return panel;
}

void ShowPanel(NSWindow* panel) {
    snapshotPanel = panel;
    // Driver tests run behind Stage Manager. Do not activate the application.
    [panel orderFront:nil];
    // A user-invoked panel may become key only while this app is already active.
    if (NSApp.isActive) [panel makeKeyWindow];
}

// A custom About window instead of the standard panel: the SDK licence asks
// for the EntisGLS copyright to be visible, and an ordinary window is both
// verifiable through the offscreen capture and consistent across the theme.
void ShowAbout() {
    if (!aboutWindow) {
        aboutWindow = Panel(@"关于缘之空", NSMakeSize(470, 330));
        aboutWindow.identifier = @"entis-about";
        NSView* content = aboutWindow.contentView;
        NSImageView* icon = [[NSImageView alloc] initWithFrame:NSMakeRect(195, 234, 80, 80)];
        icon.image = NSApp.applicationIconImage;
        icon.imageScaling = NSImageScaleProportionallyUpOrDown;
        [content addSubview:icon];
        NSTextField* name = Label(@"缘之空・悠之空", NSMakeRect(0, 198, 470, 28));
        name.font = [NSFont boldSystemFontOfSize:17];
        name.alignment = NSTextAlignmentCenter;
        [content addSubview:name];
        NSString* version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        NSString* versionLabel = version.length ? [@" " stringByAppendingString:version] : @"";
        NSTextField* subtitle = Label([NSString stringWithFormat:@"macOS 原生移植%@（EntisGLS + SDL3）", versionLabel], NSMakeRect(0, 176, 470, 20));
        subtitle.font = [NSFont systemFontOfSize:11];
        subtitle.textColor = NSColor.secondaryLabelColor;
        subtitle.alignment = NSTextAlignmentCenter;
        [content addSubview:subtitle];
        NSArray<NSString*>* lines = @[
            @"ヨスガノソラ・ハルカナソラ © Sphere",
            @"简体中文化：Sphere 中文化委员会",
            @"EntisGLS version 4 Copyright (C) 1998-2025 理影, Entis soft.",
            @"本移植为非官方个人版本，游戏素材版权归原作者所有。"
        ];
        CGFloat y = 140;
        for (NSString* line in lines) {
            NSTextField* text = Label(line, NSMakeRect(24, y, 422, 20));
            text.font = [NSFont systemFontOfSize:11];
            text.alignment = NSTextAlignmentCenter;
            text.textColor = [line hasPrefix:@"EntisGLS"] ? NSColor.labelColor : NSColor.secondaryLabelColor;
            [content addSubview:text];
            y -= 22;
        }
        NSButton* close = Button(@"好", @"close-about", NSMakeRect(185, 18, 100, 32));
        close.keyEquivalent = @"\r";
        [content addSubview:close];
    }
    [aboutWindow center];
    ShowPanel(aboutWindow);
}

void ConfirmClearData() {
    if (!dataRoot.length || clearDataPending.load()) return;
    NSAlert* first = [[NSAlert alloc] init];
    first.alertStyle = NSAlertStyleWarning;
    first.messageText = @"清除两部作品的所有存档？";
    first.informativeText = @"这会清除缘之空与悠之空的存档、快速存档、设定及攻略进度。清除后无法恢复。";
    [first addButtonWithTitle:@"取消"];
    [first addButtonWithTitle:@"继续…"];
    if ([first runModal] != NSAlertSecondButtonReturn) return;

    NSAlert* second = [[NSAlert alloc] init];
    second.alertStyle = NSAlertStyleCritical;
    second.messageText = @"最后确认：永久清除全部数据？";
    second.informativeText = @"游戏将先正常退出并结束保存，再永久清除两部作品的数据。之后请重新打开应用。";
    [second addButtonWithTitle:@"保留数据"];
    NSButton* erase = [second addButtonWithTitle:@"退出并永久清除"];
    erase.hasDestructiveAction = YES;
    erase.keyEquivalent = @"";
    if ([second runModal] != NSAlertSecondButtonReturn) return;
    clearDataPending.store(true);
    RequestQuit();
}

void SelectSettingsTab(NSString* identifier) {
    [settingsTabs selectTabViewItemWithIdentifier:identifier];
    NSArray<NSString*>* ids = @[@"general", @"shortcuts", @"data"];
    const NSUInteger index = [ids indexOfObject:identifier];
    if (index != NSNotFound) settingsTabControl.selectedSegment = NSInteger(index);
}

void ShowSettings() {
    if (!settingsWindow) {
        settingsWindow = Panel(@"设置", NSMakeSize(620, 460));
        settingsWindow.identifier = @"entis-settings";
        NSTabView* tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(18, 16, 584, 390)];
        // A normal segmented control drives the pages; the NSTabView itself is
        // borderless so the tab strip is a single native control.
        tabs.tabViewType = NSNoTabsNoBorder;
        settingsTabs = tabs;
        [settingsWindow.contentView addSubview:tabs];
        settingsTabControl = [NSSegmentedControl segmentedControlWithLabels:@[@"通用", @"快捷键", @"数据"]
            trackingMode:NSSegmentSwitchTrackingSelectOne target:controller action:@selector(settingsTabChanged:)];
        settingsTabControl.frame = NSMakeRect(188, 412, 244, 30);
        [settingsWindow.contentView addSubview:settingsTabControl];

        NSView* general = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 560, 380)];
        NSTabViewItem* generalTab = [[NSTabViewItem alloc] initWithIdentifier:@"general"];
        generalTab.label = @"通用";
        generalTab.view = general;
        [tabs addTabViewItem:generalTab];
        NSBox* display = Group(@"显示", NSMakeRect(14, 224, 532, 138), general);
        [display.contentView addSubview:Label(@"显示模式", NSMakeRect(14, 78, 110, 22))];
        displayMode = [NSSegmentedControl segmentedControlWithLabels:@[@"窗口", @"全屏幕"]
            trackingMode:NSSegmentSwitchTrackingSelectOne target:controller action:@selector(displayModeChanged:)];
        displayMode.frame = NSMakeRect(150, 75, 190, 28);
        [display.contentView addSubview:displayMode];
        [display.contentView addSubview:Label(@"启动时播放 Sphere 商标", NSMakeRect(14, 34, 270, 22))];
        startupSwitch = [[NSSwitch alloc] initWithFrame:NSMakeRect(440, 31, 42, 28)];
        startupSwitch.target = controller;
        startupSwitch.action = @selector(startupLogoChanged:);
        [display.contentView addSubview:startupSwitch];

        NSBox* game = Group(@"声音与文字", NSMakeRect(14, 92, 532, 120), general);
        [game.contentView addSubview:Label(@"在游戏设定中调整音量、语音、文字速度与自动播放。", NSMakeRect(14, 52, 484, 38))];
        [game.contentView addSubview:Button(@"打开当前游戏的设定…", @"game-config", NSMakeRect(12, 13, 230, 32))];
        NSTextField* help = Label(@"全屏幕保持原作 4:3 画面，空余区域显示黑边。\n游戏设定只在当前场景支持时打开。", NSMakeRect(28, 22, 505, 56));
        help.textColor = NSColor.secondaryLabelColor;
        [general addSubview:help];

        NSView* shortcuts = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 560, 380)];
        NSTabViewItem* keysTab = [[NSTabViewItem alloc] initWithIdentifier:@"shortcuts"];
        keysTab.label = @"快捷键";
        keysTab.view = shortcuts;
        [tabs addTabViewItem:keysTab];
        NSArray<NSArray<NSString*>*>* keys = @[
            @[@"⌘ ,", @"打开设置"], @[@"⌘ S / ⌘ L", @"存档 / 读档"],
            @[@"⇧ ⌘ S / ⇧ ⌘ L", @"快速存档 / 快速读档"],
            @[@"A", @"开启或关闭自动播放"], @[@"⌘ → / 按住 Control", @"快进 / 临时快进"],
            @[@"⌘ Y / 鼠标滚轮", @"回看记录"], @[@"H", @"隐藏对话框（点击画面恢复）"],
            @[@"Return / 鼠标左键", @"推进或确认"], @[@"Escape / 鼠标右键", @"取消或游戏菜单"],
            @[@"↑ ↓ ← →", @"选择项目"], @[@"⌃ ⌘ F", @"进入或退出全屏幕"],
            @[@"⌘ Q", @"退出并保存设定"]
        ];
        CGFloat y = 340;
        for (NSArray<NSString*>* row in keys) {
            [shortcuts addSubview:Label(row[0], NSMakeRect(28, y, 218, 23))];
            [shortcuts addSubview:Label(row[1], NSMakeRect(250, y, 280, 23))];
            y -= 25;
        }
        NSTextField* keyHelp = Label(@"游戏操作在剧情中使用；原版快捷键仍可使用。", NSMakeRect(28, 14, 505, 38));
        keyHelp.textColor = NSColor.secondaryLabelColor;
        [shortcuts addSubview:keyHelp];

        NSView* data = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 560, 380)];
        NSTabViewItem* dataTab = [[NSTabViewItem alloc] initWithIdentifier:@"data"];
        dataTab.label = @"数据";
        dataTab.view = data;
        [tabs addTabViewItem:dataTab];
        Group(@"存档与攻略进度", NSMakeRect(14, 145, 532, 216), data);
        [data addSubview:Label(@"缘之空与悠之空分别保存游戏数据。\n攻略进度随实际游玩更新，只显示已到达的内容。", NSMakeRect(40, 266, 480, 58))];
        [data addSubview:Button(@"查看攻略进度…", @"progress", NSMakeRect(36, 216, 190, 32))];
        NSButton* clearButton = Button(@"清除所有存档…", @"clear-data", NSMakeRect(36, 168, 190, 32));
        clearButton.hasDestructiveAction = YES;
        clearButton.enabled = dataRoot.length > 0;
        [data addSubview:clearButton];
        NSTextField* dataHelp = Label(@"清除操作需要连续两次确认。应用会先正常退出，再清除两部作品的存档、设定和攻略进度。", NSMakeRect(40, 72, 480, 62));
        dataHelp.textColor = NSColor.secondaryLabelColor;
        [data addSubview:dataHelp];
        NSString* home = NSHomeDirectory();
        NSString* shownRoot = [dataRoot hasPrefix:home]
            ? [@"~" stringByAppendingString:[dataRoot substringFromIndex:home.length]] : dataRoot;
        NSTextField* location = Label([NSString stringWithFormat:@"存档位置：%@", shownRoot],
            NSMakeRect(40, 20, 500, 34));
        location.font = [NSFont systemFontOfSize:11];
        location.textColor = NSColor.secondaryLabelColor;
        [data addSubview:location];
        SelectSettingsTab(@"general");
    }
    startupSwitch.state = StartupLogoEnabled() ? NSControlStateValueOn : NSControlStateValueOff;
    SyncDisplayMode();
    ShowPanel(settingsWindow);
}

NSMenuItem* MenuAction(NSString* title, NSString* key, NSEventModifierFlags modifiers, NSString* command) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:@selector(performShellAction:) keyEquivalent:key];
    item.target = controller;
    item.keyEquivalentModifierMask = modifiers;
    item.representedObject = command;
    return item;
}

void InstallMenus() {
    if (!controller) controller = [[EntisMacShellController alloc] init];
    NSMenu* menu = NSApp.mainMenu;
    if (!menu) {
        menu = [[NSMenu alloc] initWithTitle:@""];
        NSApp.mainMenu = menu;
    }
    NSMenuItem* appItem = menu.itemArray.firstObject;
    if (!appItem) {
        appItem = [[NSMenuItem alloc] initWithTitle:@"缘之空" action:nil keyEquivalent:@""];
        [menu addItem:appItem];
    }
    appItem.title = @"缘之空";
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"缘之空"];
    appItem.submenu = app;
    [app addItem:MenuAction(@"关于缘之空", @"", 0, @"about")];
    [app addItem:NSMenuItem.separatorItem];
    [app addItem:MenuAction(@"设置…", @",", NSEventModifierFlagCommand, @"settings")];
    [app addItem:MenuAction(@"攻略进度…", @"", 0, @"progress")];
    [app addItem:NSMenuItem.separatorItem];
    NSMenuItem* services = [[NSMenuItem alloc] initWithTitle:@"服务" action:nil keyEquivalent:@""];
    // SDL's existing Services menu still belongs to its original app menu.
    // AppKit forbids assigning that same submenu to a new item.
    services.submenu = [[NSMenu alloc] initWithTitle:@"服务"];
    NSApp.servicesMenu = services.submenu;
    [app addItem:services];
    [app addItem:NSMenuItem.separatorItem];
    NSMenuItem* hide = [[NSMenuItem alloc] initWithTitle:@"隐藏缘之空" action:@selector(hide:) keyEquivalent:@"h"];
    hide.target = NSApp;
    [app addItem:hide];
    NSMenuItem* hideOthers = [[NSMenuItem alloc] initWithTitle:@"隐藏其他" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    hideOthers.target = NSApp;
    [app addItem:hideOthers];
    NSMenuItem* showAll = [[NSMenuItem alloc] initWithTitle:@"全部显示" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    showAll.target = NSApp;
    [app addItem:showAll];
    [app addItem:NSMenuItem.separatorItem];
    [app addItem:MenuAction(@"退出缘之空", @"q", NSEventModifierFlagCommand, @"quit")];

    NSMenuItem* gameItem = [[NSMenuItem alloc] initWithTitle:@"游戏" action:nil keyEquivalent:@""];
    NSMenu* game = [[NSMenu alloc] initWithTitle:@"游戏"];
    gameItem.submenu = game;
    [menu insertItem:gameItem atIndex:1];
    [game addItem:MenuAction(@"存档…", @"s", NSEventModifierFlagCommand, @"ID_SAVE")];
    [game addItem:MenuAction(@"读档…", @"l", NSEventModifierFlagCommand, @"ID_LOAD")];
    [game addItem:MenuAction(@"快速存档", @"s", NSEventModifierFlagCommand | NSEventModifierFlagShift, @"ID_QSAVE")];
    [game addItem:MenuAction(@"快速读档…", @"l", NSEventModifierFlagCommand | NSEventModifierFlagShift, @"ID_QLOAD")];
    [game addItem:NSMenuItem.separatorItem];
    [game addItem:MenuAction(@"自动播放", @"a", 0, @"ID_AUTO")];
    [game addItem:MenuAction(@"快进", @"\uF703", NSEventModifierFlagCommand, @"ID_SKIP")];
    [game addItem:MenuAction(@"回看记录…", @"y", NSEventModifierFlagCommand, @"ID_HISTORY")];
    [game addItem:MenuAction(@"隐藏对话框", @"h", 0, @"ID_CLOSE")];
    [game addItem:NSMenuItem.separatorItem];
    [game addItem:MenuAction(@"游戏设定…", @"", 0, @"game-config")];

    NSMenuItem* displayItem = [[NSMenuItem alloc] initWithTitle:@"显示" action:nil keyEquivalent:@""];
    displayItem.submenu = [[NSMenu alloc] initWithTitle:@"显示"];
    [displayItem.submenu addItem:MenuAction(@"进入或退出全屏幕", @"f",
        NSEventModifierFlagCommand | NSEventModifierFlagControl, @"toggle-fullscreen")];
    [menu insertItem:displayItem atIndex:2];
    // SDL installs a stock English Window menu whose "Toggle Full Screen" item
    // would claim the same shortcut as Display's and toggle twice. Keep the
    // window utilities, drop the duplicate and use the app's language.
    for (NSMenuItem* top in menu.itemArray) {
        if (![top.title isEqual:@"Window"] || !top.submenu) continue;
        top.title = @"窗口";
        for (NSMenuItem* item in [top.submenu.itemArray copy]) {
            if (item.action == @selector(toggleFullScreen:)) { [top.submenu removeItem:item]; continue; }
            if (item.isSeparatorItem) continue;
            if ([item.title isEqual:@"Close"]) item.title = @"关闭";
            else if ([item.title isEqual:@"Close All"]) item.title = @"全部关闭";
            else if ([item.title isEqual:@"Minimize"]) item.title = @"最小化";
            else if ([item.title isEqual:@"Zoom"]) item.title = @"缩放";
        }
    }
    keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
        // Preserve text input and keyboard navigation in native panels.
        if (event.window != window) return event;
        NSEventModifierFlags flags = event.modifierFlags &
            (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
        NSString* key = event.charactersIgnoringModifiers.lowercaseString;
        if (flags == (NSEventModifierFlagCommand | NSEventModifierFlagControl) &&
            !(event.modifierFlags & NSEventModifierFlagShift) && [key isEqual:@"f"]) {
            // Consume the physical chord at the same boundary as A/H. AppKit
            // and SDL's Cocoa responder must not both toggle the same window.
            if (!event.isARepeat)
                SetFullscreen(!(window.styleMask & NSWindowStyleMaskFullScreen));
            return nil;
        }
        if (event.isARepeat || guide::Visible()) return event;
        if (!flags) {
            if ([key isEqual:@"a"] || [key isEqual:@"h"]) {
                PostGameCommand([key isEqual:@"a"] ? @"ID_AUTO" : @"ID_CLOSE");
                return nil;
            }
        }
        return event;
    }];
}
}

// The GL viewport adds black bars at the sides in full screen. Windowed
// resizing retains the game's 4:3 content ratio.
void FitWindowToScreen(NSWindow* nsWindow) {
    NSScreen* screen = nsWindow.screen ?: NSScreen.mainScreen;
    const NSRect visible = screen.visibleFrame;
    const CGFloat titleBar = NSHeight(nsWindow.frame) - NSHeight(nsWindow.contentLayoutRect);
    CGFloat height = std::floor((NSHeight(visible) - titleBar) * 0.92);
    CGFloat width = std::floor(height * 4 / 3);
    if (width > NSWidth(visible) * 0.95) { width = std::floor(NSWidth(visible) * 0.95); height = std::floor(width * 3 / 4); }
    NSRect frame = [nsWindow frameRectForContentRect:NSMakeRect(0, 0, width, height)];
    frame.origin.x = NSMidX(visible) - NSWidth(frame) / 2;
    frame.origin.y = NSMidY(visible) - NSHeight(frame) / 2;
    [nsWindow setFrame:frame display:YES];
}

void AttachToWindow(SDL_Window* sdlWindow) {
    NSWindow* nsWindow = WindowOf(sdlWindow);
    const bool first = nsWindow != window;
    window = nsWindow;
    if (!window) return;
    window.contentView.wantsLayer = YES;
    if (!controller) InstallMenus();
    if (!first) return;
    window.contentAspectRatio = NSMakeSize(4, 3);
    window.contentMinSize = NSMakeSize(640, 480);
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    window.backgroundColor = NSColor.blackColor;
    FitWindowToScreen(window);
}

void ConfigureDataRoot(const char* path) {
    NSString* copy = path ? [NSString stringWithUTF8String:path] : nil;
    OnMain(^{
        dataRoot = [copy stringByStandardizingPath];
        guide::SetDataRoot(dataRoot);
    });
}

bool StartupLogoEnabled() {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    id stored = [defaults objectForKey:startupLogoKey];
    return stored ? [defaults boolForKey:startupLogoKey] : true;
}

void OnHostEvent(const Event& event) {
    const Event copy = event;
    OnMain(^{
        if (copy.kind == Event::Kind::SessionStarted && window)
            window.title = copy.game == Game::Haruka ? @"悠之空" : @"缘之空";
        if (copy.kind == Event::Kind::GuideRequested)
            guide::Show(window, copy.game == Game::Haruka ? @"haruka" : @"yosuga");
        else if (guide::Visible()) guide::Refresh();
    });
}

void HandleShellCommand(const char* command) {
    NSString* copy = command ? [NSString stringWithUTF8String:command] : @"";
    OnMain(^{
        if ([copy isEqual:@"settings"] || [copy isEqual:@"settings-shortcuts"] || [copy isEqual:@"settings-data"]) {
            ShowSettings();
            SelectSettingsTab([copy isEqual:@"settings-shortcuts"] ? @"shortcuts" :
                ([copy isEqual:@"settings-data"] ? @"data" : @"general"));
        }
        else if ([copy hasPrefix:@"progress"] && guide::HandleCommand(window, copy)) {
            // The page lives in the game window; from Settings, return to it.
            if (settingsWindow.isVisible) {
                [settingsWindow orderOut:nil];
                [window makeKeyAndOrderFront:nil];
            }
        }
        else if (guide::HandleCommand(window, copy)) {}
        else if ([copy isEqual:@"close-settings"]) [settingsWindow close];
        else if ([copy isEqual:@"close-about"]) [aboutWindow close];
        else if ([copy isEqual:@"about"]) ShowAbout();
        else if ([copy isEqual:@"fullscreen"]) SetFullscreen(true);
        else if ([copy isEqual:@"windowed"]) SetFullscreen(false);
        else if ([copy isEqual:@"toggle-fullscreen"]) SetFullscreen(!(window.styleMask & NSWindowStyleMaskFullScreen));
        else if ([copy isEqual:@"test-wide"]) TestWindowAspect(true);
        else if ([copy isEqual:@"test-restore"]) TestWindowAspect(false);
        else if ([copy isEqual:@"game-config"] || [copy isEqual:@"settings-game-config"]) {
            [settingsWindow orderOut:nil];
            PostGameCommand(@"ID_CONFIG");
        }
        else if ([copy isEqual:@"clear-data"]) ConfirmClearData();
        else if ([copy isEqual:@"clear-data-force"]) {
            // Test-only path past the modal alerts; requires both isolated
            // development inputs, exactly like the other diagnostics.
            const char* driver = SDL_getenv("ENTISGLS_DRIVER");
            const char* saves = SDL_getenv("ENTISGLS_TEST_SAVEDIR");
            if (driver && *driver && saves && *saves && dataRoot.length) {
                clearDataPending.store(true);
                RequestQuit();
            }
        }
        else if ([copy isEqual:@"quit"]) RequestQuit();
        else if ([copy hasPrefix:@"ID_"]) PostGameCommand(copy);
    });
}

bool ClearSavedDataAfterExit() {
    if (!clearDataPending.exchange(false)) return false;
    NSFileManager* files = NSFileManager.defaultManager;
    NSMutableArray<NSString*>* failures = [NSMutableArray array];
    NSString* root = dataRoot.stringByStandardizingPath;
    if (!root.isAbsolutePath || [root isEqual:@"/"] || !root.length) {
        [failures addObject:@"存档目录无效，未清除数据。"];
    } else {
        NSString* canonicalRoot = root.stringByResolvingSymlinksInPath;
        for (NSString* game in @[@"yosuga", @"haruka"]) {
            NSString* folder = [[root stringByAppendingPathComponent:game] stringByAppendingPathComponent:@"savedata"];
            NSString* canonical = folder.stringByResolvingSymlinksInPath;
            NSString* expected = [[canonicalRoot stringByAppendingPathComponent:game] stringByAppendingPathComponent:@"savedata"];
            if (![canonical isEqual:expected]) {
                [failures addObject:[NSString stringWithFormat:@"%@：存档目录是符号链接，未清除。", game]];
                continue;
            }
            BOOL directory = NO;
            if (![files fileExistsAtPath:folder isDirectory:&directory]) continue;
            if (!directory) {
                [failures addObject:[NSString stringWithFormat:@"%@：存档路径不是文件夹。", game]];
                continue;
            }
            NSError* error = nil;
            NSArray<NSString*>* children = [files contentsOfDirectoryAtPath:folder error:&error];
            if (!children) {
                [failures addObject:error.localizedDescription ?: @"无法读取存档目录。"];
                continue;
            }
            for (NSString* child in children) {
                if (![files removeItemAtPath:[folder stringByAppendingPathComponent:child] error:&error])
                    [failures addObject:error.localizedDescription ?: @"无法删除存档文件。"];
            }
        }
    }
    if (failures.count) OnMain(^{
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = @"部分数据未能清除";
        alert.informativeText = [failures componentsJoinedByString:@"\n"];
        [alert addButtonWithTitle:@"好"];
        [alert runModal];
    });
    return true;
}

void SaveOverlaySnapshot(const char* path) {
    const std::string savedPath = path ?: "";
    dispatch_block_t capture = ^{
        NSWindow* panel = snapshotPanel.isVisible ? snapshotPanel :
            (settingsWindow.isVisible ? settingsWindow : nil);
        if (panel.attachedSheet) panel = panel.attachedSheet;
        NSView* content = panel ? panel.contentView : window.contentView;
        if (!content) return;
        __block NSBitmapImageRep* rep = nil;
        if (panel) {
            // Cached layer contents can be transparent or contain unresolved
            // semantic colors while Stage Manager has parked this window.
            // Redraw this app's view subtree in its own RGB context instead;
            // neither window-server capture nor activation is involved.
            [content layoutSubtreeIfNeeded];
            const CGFloat scale = std::max<CGFloat>(1, panel.backingScaleFactor);
            const NSInteger width = NSInteger(std::ceil(NSWidth(content.bounds) * scale));
            const NSInteger height = NSInteger(std::ceil(NSHeight(content.bounds) * scale));
            if (width <= 0 || height <= 0) return;
            const auto bitmap = ^NSBitmapImageRep* {
                NSBitmapImageRep* image = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                    pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
                    isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
                image.size = content.bounds.size;
                return image;
            };
            NSBitmapImageRep* drawn = bitmap();
            rep = bitmap();
            [panel.effectiveAppearance performAsCurrentDrawingAppearance:^{
                NSGraphicsContext* drawing = [NSGraphicsContext graphicsContextWithBitmapImageRep:drawn];
                [NSGraphicsContext saveGraphicsState];
                NSGraphicsContext.currentContext = drawing;
                [NSColor.clearColor setFill];
                NSRectFillUsingOperation(content.bounds, NSCompositingOperationCopy);
                [content displayRectIgnoringOpacity:content.bounds inContext:drawing];
                [NSGraphicsContext restoreGraphicsState];

                // Some standard controls intentionally erase portions of their
                // backing layer. Composite them over the window's actual
                // background so the exported PNG is completely opaque.
                [NSGraphicsContext saveGraphicsState];
                NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
                [NSColor.windowBackgroundColor setFill];
                NSRectFillUsingOperation(content.bounds, NSCompositingOperationCopy);
                [drawn drawInRect:content.bounds fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                    fraction:1 respectFlipped:YES hints:nil];
                [NSGraphicsContext restoreGraphicsState];
            }];
        } else {
            rep = [content bitmapImageRepForCachingDisplayInRect:content.bounds];
            [NSGraphicsContext saveGraphicsState];
            NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
            [NSColor.clearColor setFill];
            NSRectFillUsingOperation(content.bounds, NSCompositingOperationCopy);
            [NSGraphicsContext restoreGraphicsState];
            for (NSView* view in content.subviews) {
                if (view.hidden || view.alphaValue <= 0.01) continue;
                NSBitmapImageRep* part = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
                [view cacheDisplayInRect:view.bounds toBitmapImageRep:part];
                [NSGraphicsContext saveGraphicsState];
                NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
                [part drawInRect:view.frame fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                        fraction:view.alphaValue respectFlipped:YES hints:nil];
                [NSGraphicsContext restoreGraphicsState];
            }
        }
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        [png writeToFile:[NSString stringWithUTF8String:savedPath.c_str()] atomically:YES];
    };
    if (NSThread.isMainThread) capture();
    else dispatch_sync(dispatch_get_main_queue(), capture);
}

// Development aid: the menu bar cannot be captured offscreen without the
// window server, so dump the actual NSApp.mainMenu tree instead.
void DumpMenus(const char* path) {
    const std::string savedPath = path ? path : "";
    if (savedPath.empty()) return;
    dispatch_block_t dump = ^{
        NSMutableString* text = [NSMutableString string];
        for (NSMenuItem* top in NSApp.mainMenu.itemArray) {
            [text appendFormat:@"MENU %@\n", top.title];
            if (!top.submenu) continue;
            for (NSMenuItem* item in top.submenu.itemArray) {
                if (item.isSeparatorItem) { [text appendString:@"  ---\n"]; continue; }
                [text appendFormat:@"  %@ | key=%@ | modifiers=%lu | action=%@ | enabled=%d\n",
                    item.title, item.keyEquivalent ?: @"",
                    (unsigned long)item.keyEquivalentModifierMask,
                    item.action ? NSStringFromSelector(item.action) : @"none",
                    item.isEnabled ? 1 : 0];
            }
        }
        [text writeToFile:[NSString stringWithUTF8String:savedPath.c_str()] atomically:YES
            encoding:NSUTF8StringEncoding error:nil];
    };
    if (NSThread.isMainThread) dump(); else dispatch_sync(dispatch_get_main_queue(), dump);
}
}

@implementation EntisMacShellController
- (void)performShellAction:(id)sender {
    NSString* command = [sender isKindOfClass:NSMenuItem.class] ? [sender representedObject] : [sender identifier];
    entis::mac::HandleShellCommand(command.UTF8String);
}
- (void)startupLogoChanged:(id)sender {
    const bool enabled = [sender state] == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:enabled
        forKey:entis::mac::startupLogoKey];
    Bridge::Get().SetStartupLogoEnabled(enabled);
}
- (void)displayModeChanged:(id)sender {
    entis::mac::SetFullscreen([sender selectedSegment] == 1);
}
- (void)settingsTabChanged:(id)sender {
    NSArray<NSString*>* ids = @[@"general", @"shortcuts", @"data"];
    const NSInteger index = [sender selectedSegment];
    if (index >= 0 && index < NSInteger(ids.count)) entis::mac::SelectSettingsTab(ids[index]);
}
@end
