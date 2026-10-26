#include "apps/macos/guide_overlay.h"
#include "runtime/cotopha_port/game_progress.h"
#include "runtime/cotopha_port/host_bridge.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using entis::host::Bridge;
using entis::host::Game;

@interface EntisGuideOverlay : NSView
@end

namespace entis::mac::guide {
namespace {

// ---- 空澄 palette and type: the bright system pages of mods/src/sky_ui.py
NSColor* Color(int r, int g, int b, CGFloat a = 1) {
    return [NSColor colorWithSRGBRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:a];
}
NSColor* Ink(CGFloat a = 1) { return Color(24, 66, 92, a); }
NSColor* InkSub(CGFloat a = 1) { return Color(86, 124, 146, a); }
NSColor* InkFaint(CGFloat a = 1) { return Color(140, 170, 188, a); }
NSColor* TealInk(CGFloat a = 1) { return Color(10, 52, 72, a); }
NSColor* Line(CGFloat a = 1) { return Color(126, 186, 214, a); }
NSColor* LineSoft(CGFloat a = 1) { return Color(186, 220, 236, a); }
NSColor* Accent(CGFloat a = 1) { return Color(52, 152, 204, a); }
NSColor* AccentDeep(CGFloat a = 1) { return Color(30, 112, 162, a); }
NSColor* AccentTop(CGFloat a = 1) { return Color(88, 180, 224, a); }
NSColor* AccentBottom(CGFloat a = 1) { return Color(44, 130, 186, a); }
NSColor* AccentSoft(CGFloat a = 1) { return Color(214, 238, 249, a); }
NSColor* Paper(CGFloat a = 1) { return Color(255, 255, 255, a); }
NSColor* Mist(CGFloat a = 1) { return Color(238, 247, 252, a); }
NSColor* Coral(CGFloat a = 1) { return Color(232, 96, 78, a); }
NSColor* Mint(CGFloat a = 1) { return Color(200, 240, 224, a); }

NSFont* Round(CGFloat size, bool bold = false) {
    NSFont* font = [NSFont fontWithName:bold ? @"STYuanti-SC-Bold" : @"STYuanti-SC-Regular" size:size];
    if (!font) font = [NSFont fontWithName:bold ? @"PingFangSC-Semibold" : @"PingFangSC-Regular" size:size];
    return font ?: [NSFont systemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular];
}
NSFont* Latin(CGFloat size, NSFontWeight weight = NSFontWeightSemibold) {
    NSFont* base = [NSFont systemFontOfSize:size weight:weight];
    NSFontDescriptor* rounded = [base.fontDescriptor fontDescriptorWithDesign:NSFontDescriptorSystemDesignRounded];
    return rounded ? ([NSFont fontWithDescriptor:rounded size:size] ?: base) : base;
}

// ---- logical layout (the game's 800x600 picture)
const NSRect kScreen = NSMakeRect(0, 0, 800, 600);
const NSRect kBoard = NSMakeRect(24, 90, 516, 458);
const NSRect kDetail = NSMakeRect(552, 90, 224, 458);
const NSRect kGraph = NSMakeRect(30, 124, 504, 418);
const NSRect kList = NSMakeRect(30, 98, 504, 444);
constexpr CGFloat kCardW = 236, kCardH = 50, kPlateW = 116, kPlateH = 40, kRowH = 156;

// ---- record helpers (the same filtering rules as the earlier native panel)
NSDictionary* Dictionary(id value) { return [value isKindOfClass:NSDictionary.class] ? value : @{}; }
NSArray* Array(id value) { return [value isKindOfClass:NSArray.class] ? value : @[]; }
NSString* Text(id value, NSString* fallback = @"") { return [value isKindOfClass:NSString.class] ? value : fallback; }
bool Boolean(id value) { return [value isKindOfClass:NSNumber.class] && [value boolValue]; }
long long Count(id value) { return [value isKindOfClass:NSNumber.class] ? std::max(0LL, [value longLongValue]) : 0; }

struct Hot { NSRect rect; NSString* action; };
struct Box { NSString* identifier; NSRect card; std::vector<std::pair<long long, NSRect>> plates; };

EntisGuideOverlay* overlay = nil;
NSWindow* host = nil;
__weak NSResponder* previousResponder = nil;  // the game view, given back on close
NSString* dataRoot = nil;
NSString* game = @"yosuga";
NSInteger page = 0;
NSDictionary* record = @{};
NSDictionary* summary = nil;
std::string lastGuide, lastSummary;
NSString* readError = @"";
NSString* localError = @"";
NSString* statusNote = @"";
NSString* selectedKind = @"";
NSString* selectedID = @"";
NSString* selectedCheckpoint = @"";
bool confirming = false, awaiting = false;
NSString* confirmKind = @"", *confirmID = @"", *confirmCheckpoint = @"";
CGPoint pan = CGPointZero;
CGFloat zoom = 0.9;
bool placed = false;
CGFloat listScroll = 0, listExtent = 0;
std::vector<Hot> hots;
NSString* hover = nil;
NSString* pressed = nil;
bool panning = false;
NSPoint panStart = NSZeroPoint;
CGPoint panOrigin = CGPointZero;
std::vector<Box> boxes;
NSArray<NSDictionary*>* ordered = @[];
NSArray<NSDictionary*>* edges = @[];
NSRect graphBounds = NSZeroRect;
NSTimer* timer = nil;

bool DriverEnabled(bool isolated = false) {
    const char* driver = SDL_getenv("ENTISGLS_DRIVER");
    const char* saves = SDL_getenv("ENTISGLS_TEST_SAVEDIR");
    return driver && *driver && (!isolated || (saves && *saves));
}

bool ActionPending() {
    NSString* state = Text(Dictionary(record[@"action"])[@"state"]);
    return [@[@"pending", @"preparing", @"prepared", @"restoring"] containsObject:state];
}
bool Completed(NSDictionary* item) { return Boolean(item[@"completed"]) || Boolean(item[@"complete"]); }

NSArray<NSDictionary*>* ReachedNodes() {
    NSMutableArray* result = [NSMutableArray array];
    for (id value in Array(record[@"nodes"])) {
        NSDictionary* node = Dictionary(value);
        if (Boolean(node[@"visited"]) && Text(node[@"id"]).length) [result addObject:node];
    }
    return result;
}
NSArray<NSDictionary*>* VisibleOptions(NSDictionary* node) {
    NSMutableArray* result = [NSMutableArray array];
    for (id value in Array(node[@"options"])) {
        NSDictionary* option = Dictionary(value);
        // Only options the runtime actually supplied; never reveal a hidden one.
        if (option.count && (!option[@"visible"] || Boolean(option[@"visible"]))) [result addObject:option];
    }
    return result;
}
NSDictionary* FindItem(NSString* kind, NSString* identifier) {
    NSString* key = [kind isEqual:@"node"] ? @"nodes" :
        ([kind isEqual:@"chapter"] ? @"chapters" : ([kind isEqual:@"ending"] ? @"endings" : @"specials"));
    for (id value in Array(record[key])) {
        NSDictionary* item = Dictionary(value);
        if (![Text(item[@"id"]) isEqual:identifier]) continue;
        if ([kind isEqual:@"node"] && !Boolean(item[@"visited"])) return nil;
        if ([kind isEqual:@"ending"] && !Completed(item)) return nil;
        if (([kind isEqual:@"chapter"] || [kind isEqual:@"special"]) && !Boolean(item[@"unlocked"])) return nil;
        return item;
    }
    return nil;
}
NSDictionary* SummaryRecord() {
    const std::string live = entis::progress::LatestProgressJson(game.UTF8String);
    NSData* data = live.empty() ? nil : [NSData dataWithBytes:live.data() length:live.size()];
    if (!data && dataRoot.length) {
        NSString* path = [[[dataRoot stringByAppendingPathComponent:game]
            stringByAppendingPathComponent:@"savedata"] stringByAppendingPathComponent:@"progress.json"];
        data = [NSData dataWithContentsOfFile:path];
    }
    if (!data) return nil;
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![value isKindOfClass:NSDictionary.class] || ![value[@"game"] isEqual:game] ||
        [value[@"schema_version"] integerValue] != 1) return nil;
    return value;
}
NSString* DateText(id value) {
    double timestamp = [value isKindOfClass:NSNumber.class] ? [value doubleValue] : 0;
    if (timestamp > 1e12) timestamp /= 1000;
    if (timestamp <= 0) return @"时间未知";
    static NSDateFormatter* formatter = [] {
        NSDateFormatter* f = [[NSDateFormatter alloc] init];
        f.dateFormat = @"M月d日 HH:mm";
        return f;
    }();
    return [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:timestamp]];
}

// ---- drawing primitives (logical coordinates, flipped)
NSBezierPath* Rounded(NSRect rect, CGFloat radius) {
    return [NSBezierPath bezierPathWithRoundedRect:rect xRadius:radius yRadius:radius];
}
void Fill(NSRect rect, CGFloat radius, NSColor* color) { [color setFill]; [Rounded(rect, radius) fill]; }
void Stroke(NSRect rect, CGFloat radius, NSColor* color, CGFloat width = 0.8) {
    NSBezierPath* path = Rounded(NSInsetRect(rect, width / 2, width / 2), radius);
    path.lineWidth = width;
    [color setStroke];
    [path stroke];
}
void Vertical(NSRect rect, CGFloat radius, NSColor* top, NSColor* bottom) {
    // In this flipped view angle 90 runs from the top edge to the bottom edge.
    [[[NSGradient alloc] initWithStartingColor:top endingColor:bottom] drawInBezierPath:Rounded(rect, radius) angle:90];
}
void Hairline(CGFloat x0, CGFloat x1, CGFloat y, NSColor* color, CGFloat width = 0.8) {
    NSGradient* gradient = [[NSGradient alloc] initWithColors:@[[color colorWithAlphaComponent:0], color, color,
        [color colorWithAlphaComponent:0]] atLocations:(const CGFloat[]){0, 0.18, 0.82, 1}
        colorSpace:NSColorSpace.sRGBColorSpace];
    [gradient drawInRect:NSMakeRect(x0, y - width / 2, x1 - x0, width) angle:0];
}
void Glass(NSRect rect, CGFloat radius, NSColor* top, NSColor* bottom, NSColor* border, CGFloat highlight = 0.45) {
    Vertical(rect, radius, top, bottom);
    if (border) Stroke(rect, radius, border, 0.8);
    if (highlight > 0) Hairline(NSMinX(rect) + radius, NSMaxX(rect) - radius, NSMinY(rect) + 1.2, Paper(highlight), 0.8);
}
NSDictionary* Attributes(NSFont* font, NSColor* color, NSTextAlignment alignment = NSTextAlignmentLeft,
                         CGFloat kern = 0, bool shadow = false, CGFloat lineSpacing = 2) {
    NSMutableParagraphStyle* paragraph = [[NSMutableParagraphStyle alloc] init];
    paragraph.alignment = alignment;
    paragraph.lineBreakMode = NSLineBreakByWordWrapping;
    paragraph.lineSpacing = lineSpacing;
    NSMutableDictionary* attributes = [@{NSFontAttributeName: font, NSForegroundColorAttributeName: color,
                                         NSParagraphStyleAttributeName: paragraph} mutableCopy];
    if (kern) attributes[NSKernAttributeName] = @(kern);
    if (shadow) {
        NSShadow* soft = [[NSShadow alloc] init];
        soft.shadowColor = TealInk(0.55);
        soft.shadowOffset = NSMakeSize(0, -0.8);
        soft.shadowBlurRadius = 1.6;
        attributes[NSShadowAttributeName] = soft;
    }
    return attributes;
}
void Draw(NSString* text, NSRect rect, NSDictionary* attributes) {
    [text drawWithRect:rect options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
            attributes:attributes context:nil];
}
CGFloat Measure(NSString* text, CGFloat width, NSDictionary* attributes) {
    return std::ceil(NSHeight([text boundingRectWithSize:NSMakeSize(width, CGFLOAT_MAX)
        options:NSStringDrawingUsesLineFragmentOrigin attributes:attributes context:nil]));
}
CGFloat Width(NSString* text, NSDictionary* attributes) { return std::ceil([text sizeWithAttributes:attributes].width); }

void AddHot(NSRect rect, NSString* action, NSRect clip = kScreen) {
    const NSRect visible = NSIntersectionRect(rect, clip);
    if (!NSIsEmptyRect(visible)) hots.push_back({visible, action});
}
bool Hovered(NSString* action) { return hover && [hover isEqual:action]; }

enum class Style { Primary, Secondary, Tab, TabActive, Quiet };
void Button(NSRect rect, NSString* label, NSString* action, Style style, bool enabled = true, NSString* caption = nil) {
    const bool over = enabled && Hovered(action);
    const CGFloat radius = NSHeight(rect) / 2;
    NSColor* ink = Paper();
    switch (style) {
    case Style::Primary:
        if (enabled) {
            if (over) Fill(NSInsetRect(rect, -2, -2), radius + 2, Accent(0.18));
            Glass(rect, radius, over ? Color(104, 192, 232) : AccentTop(), over ? Color(54, 142, 198) : AccentBottom(),
                  Paper(over ? 0.95 : 0.75), 0.6);
        } else {
            Fill(rect, radius, Mist());
            Stroke(rect, radius, LineSoft());
            ink = InkFaint();
        }
        break;
    case Style::Secondary:
        Fill(rect, radius, over ? AccentSoft() : Paper(0.96));
        Stroke(rect, radius, over ? Accent() : Line(0.85));
        ink = enabled ? Ink() : InkFaint();
        break;
    case Style::TabActive:
        Glass(rect, radius, AccentTop(), AccentBottom(), Paper(0.8), 0.6);
        break;
    case Style::Tab:
        if (over) Fill(rect, radius, AccentSoft());
        ink = over ? Ink() : InkSub();
        break;
    case Style::Quiet:
        ink = over ? AccentDeep() : InkSub();
        break;
    }
    NSDictionary* attributes = Attributes(Round(NSHeight(rect) >= 30 ? 13.5 : 12, style != Style::Quiet), ink,
                                          NSTextAlignmentCenter);
    const CGFloat textHeight = Measure(label, NSWidth(rect), attributes);
    CGFloat top = NSMidY(rect) - textHeight / 2 - (caption ? 4 : 0);
    Draw(label, NSMakeRect(NSMinX(rect), top, NSWidth(rect), textHeight + 2), attributes);
    if (caption) {
        NSDictionary* small = Attributes(Latin(5.8), [ink colorWithAlphaComponent:0.7], NSTextAlignmentCenter, 1.2, false);
        Draw(caption, NSMakeRect(NSMinX(rect), top + textHeight - 1, NSWidth(rect), 9), small);
    }
    if (enabled) AddHot(rect, action);
}

// ---- data
void ParseRecord() {
    const std::string json = entis::progress::LatestGuideJson(game.UTF8String);
    const std::string stats = entis::progress::LatestProgressJson(game.UTF8String);
    lastGuide = json;
    lastSummary = stats;
    NSError* error = nil;
    id decoded = json.empty() ? nil : [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json.data() length:json.size()]
                                                                       options:0 error:&error];
    NSDictionary* value = Dictionary(decoded);
    readError = @"";
    if (!json.empty() && ![decoded isKindOfClass:NSDictionary.class] && !error) {
        readError = @"攻略记录不是有效的对象，未显示其内容。";
    } else if (value.count && ([value[@"schema_version"] integerValue] != 1 || ![Text(value[@"game"]) isEqual:game])) {
        value = @{};
        readError = @"攻略记录的版本或作品标识不匹配，未显示其内容。";
    } else if (error) {
        readError = [@"攻略记录无法读取：" stringByAppendingString:error.localizedDescription];
    }
    record = value;
    summary = SummaryRecord();
    if (!FindItem(selectedKind, selectedID)) selectedKind = selectedID = selectedCheckpoint = @"";
}

// Story order: scenario, then the line count where the choice appeared. Rows
// follow the story even where traversal links are missing (loads reset them).
void LayoutFlow() {
    NSArray<NSDictionary*>* nodes = [ReachedNodes() sortedArrayUsingComparator:^NSComparisonResult(NSDictionary* a, NSDictionary* b) {
        NSComparisonResult scene = [Text(a[@"scenario"]) compare:Text(b[@"scenario"]) options:NSNumericSearch];
        if (scene != NSOrderedSame) return scene;
        const long long x = Count(a[@"ordinal"]), y = Count(b[@"ordinal"]);
        return x < y ? NSOrderedAscending : (x > y ? NSOrderedDescending : NSOrderedSame);
    }];
    NSMutableDictionary<NSString*, NSDictionary*>* byID = [NSMutableDictionary dictionary];
    for (NSDictionary* node in nodes) byID[Text(node[@"id"])] = node;
    NSMutableArray* kept = [NSMutableArray array];
    for (id value in Array(record[@"edges"])) {
        NSDictionary* edge = Dictionary(value);
        NSDictionary* source = byID[Text(edge[@"source_id"])];
        if (!source || !byID[Text(edge[@"target_id"])]) continue;
        bool chosen = false;
        for (NSDictionary* option in VisibleOptions(source))
            if (Boolean(option[@"chosen"]) && Count(option[@"index"]) == Count(edge[@"option_index"])) chosen = true;
        if (chosen) [kept addObject:edge];
    }
    ordered = nodes;
    edges = kept;
    boxes.clear();
    std::map<std::string, int> rows;
    std::map<int, int> columns;
    int row = -1;
    NSString* previousKey = nil;
    graphBounds = NSZeroRect;
    for (NSDictionary* node in nodes) {
        NSString* key = [NSString stringWithFormat:@"%@#%lld", Text(node[@"scenario"]), Count(node[@"ordinal"])];
        if (![key isEqual:previousKey]) { ++row; previousKey = key; }
        const int column = columns[row]++;
        Box box;
        box.identifier = Text(node[@"id"]);
        box.card = NSMakeRect(column * (kCardW + 64), row * kRowH, kCardW, kCardH);
        NSArray* options = VisibleOptions(node);
        const CGFloat total = options.count * kPlateW + (options.count ? (options.count - 1) * 8 : 0);
        CGFloat x = NSMidX(box.card) - total / 2;
        for (NSDictionary* option in options) {
            box.plates.push_back({Count(option[@"index"]), NSMakeRect(x, NSMaxY(box.card) + 22, kPlateW, kPlateH)});
            x += kPlateW + 8;
        }
        NSRect extent = box.card;
        for (const auto& plate : box.plates) extent = NSUnionRect(extent, plate.second);
        graphBounds = NSIsEmptyRect(graphBounds) ? extent : NSUnionRect(graphBounds, extent);
        boxes.push_back(box);
    }
}
const Box* FindBox(NSString* identifier) {
    for (const auto& box : boxes) if ([box.identifier isEqual:identifier]) return &box;
    return nullptr;
}
NSPoint ToScreen(NSPoint graph) {
    return NSMakePoint(NSMinX(kGraph) + pan.x + graph.x * zoom, NSMinY(kGraph) + pan.y + graph.y * zoom);
}
NSRect ToScreen(NSRect graph) {
    const NSPoint origin = ToScreen(graph.origin);
    return NSMakeRect(origin.x, origin.y, NSWidth(graph) * zoom, NSHeight(graph) * zoom);
}
void Fit() {
    if (NSIsEmptyRect(graphBounds)) { zoom = 0.9; pan = CGPointMake(20, 16); return; }
    const NSRect bounds = NSInsetRect(graphBounds, -16, -16);
    zoom = std::clamp(std::min(NSWidth(kGraph) / NSWidth(bounds), NSHeight(kGraph) / NSHeight(bounds)), 0.55, 1.0);
    pan = CGPointMake((NSWidth(kGraph) - NSWidth(bounds) * zoom) / 2 - NSMinX(bounds) * zoom,
                      std::max<CGFloat>(10, (NSHeight(kGraph) - NSHeight(bounds) * zoom) / 2) - NSMinY(bounds) * zoom);
}
void Focus(NSString* identifier) {
    const Box* box = FindBox(identifier);
    if (!box) return;
    pan = CGPointMake(NSWidth(kGraph) / 2 - NSMidX(box->card) * zoom, 40 - NSMinY(box->card) * zoom);
}
void ClampPan() {
    if (NSIsEmptyRect(graphBounds)) return;
    const NSRect b = ToScreen(NSInsetRect(graphBounds, -20, -20));
    if (NSMaxX(b) < NSMinX(kGraph) + 80) pan.x += NSMinX(kGraph) + 80 - NSMaxX(b);
    if (NSMinX(b) > NSMaxX(kGraph) - 80) pan.x -= NSMinX(b) - (NSMaxX(kGraph) - 80);
    if (NSMaxY(b) < NSMinY(kGraph) + 60) pan.y += NSMinY(kGraph) + 60 - NSMaxY(b);
    if (NSMinY(b) > NSMaxY(kGraph) - 60) pan.y -= NSMinY(b) - (NSMaxY(kGraph) - 60);
}

void Select(NSString* kind, NSString* identifier) {
    selectedKind = kind ?: @"";
    selectedID = identifier ?: @"";
    selectedCheckpoint = @"";
    localError = @"";
}
void Redraw() { overlay.needsDisplay = YES; }

// ---- pages
void DrawFlow() {
    NSDictionary* small = Attributes(Round(11), InkSub());
    NSUInteger restorable = 0;
    for (NSDictionary* node in ordered) if (Boolean(node[@"can_restore"])) ++restorable;
    Draw([NSString stringWithFormat:@"已到达 %lu 个选择 · 可以回到 %lu 个", (unsigned long)ordered.count, (unsigned long)restorable],
         NSMakeRect(40, 99, 260, 16), small);
    if (ordered.count) {
        Button(NSMakeRect(318, 96, 66, 20), @"定位最近", @"locate", Style::Quiet);
        Button(NSMakeRect(388, 96, 64, 20), @"‹ 上一个", @"previous", Style::Quiet);
        Button(NSMakeRect(456, 96, 64, 20), @"下一个 ›", @"next", Style::Quiet);
    }
    Hairline(36, 528, 119.5, Line(0.7));
    if (!ordered.count) {
        Draw(@"还没有到达选择。\n继续游玩后，这里会记录实际出现的选项和走过的分支。",
             NSMakeRect(60, 270, 444, 60), Attributes(Round(13), InkSub(), NSTextAlignmentCenter, 0, false, 6));
        return;
    }
    [NSGraphicsContext saveGraphicsState];
    [[NSBezierPath bezierPathWithRect:kGraph] addClip];
    // Links that actually happened: the chosen option to the next choice reached.
    for (NSDictionary* edge in edges) {
        const Box* source = FindBox(Text(edge[@"source_id"]));
        const Box* target = FindBox(Text(edge[@"target_id"]));
        if (!source || !target) continue;
        NSRect plate = NSZeroRect;
        for (const auto& p : source->plates) if (p.first == Count(edge[@"option_index"])) plate = p.second;
        if (NSIsEmptyRect(plate)) continue;
        const NSPoint from = ToScreen(NSMakePoint(NSMidX(plate), NSMaxY(plate)));
        const NSPoint to = ToScreen(NSMakePoint(NSMidX(target->card), NSMinY(target->card)));
        NSBezierPath* path = [NSBezierPath bezierPath];
        path.lineWidth = 1.8 * zoom;
        path.lineJoinStyle = NSLineJoinStyleRound;
        path.lineCapStyle = NSLineCapStyleRound;
        [path moveToPoint:from];
        if (to.y > from.y + 8) {
            const CGFloat bend = from.y + 14 * zoom;
            [path lineToPoint:NSMakePoint(from.x, bend)];
            [path lineToPoint:NSMakePoint(to.x, bend)];
            [path lineToPoint:to];
        } else {
            [path curveToPoint:to controlPoint1:NSMakePoint(from.x + 160 * zoom, from.y)
                 controlPoint2:NSMakePoint(to.x + 160 * zoom, to.y - 40 * zoom)];
        }
        [Accent(0.9) setStroke];
        [path stroke];
        NSBezierPath* arrow = [NSBezierPath bezierPath];
        arrow.lineWidth = 1.6 * zoom;
        [arrow moveToPoint:NSMakePoint(to.x - 4 * zoom, to.y - 6 * zoom)];
        [arrow lineToPoint:to];
        [arrow lineToPoint:NSMakePoint(to.x + 4 * zoom, to.y - 6 * zoom)];
        [arrow stroke];
    }
    NSUInteger index = 0;
    for (NSDictionary* node in ordered) {
        const Box* box = FindBox(Text(node[@"id"]));
        ++index;
        if (!box) continue;
        NSString* action = [@"select:node:" stringByAppendingString:box->identifier];
        const NSRect card = ToScreen(box->card);
        const bool selected = [selectedKind isEqual:@"node"] && [selectedID isEqual:box->identifier];
        const bool over = Hovered(action);
        const CGFloat radius = 9 * zoom;
        if (selected) {
            Fill(NSInsetRect(card, -3 * zoom, -3 * zoom), radius + 3 * zoom, Accent(0.16));
            Glass(card, radius, AccentTop(), AccentBottom(), Paper(0.9), 0.7);
            NSBezierPath* marker = [NSBezierPath bezierPath];
            [marker moveToPoint:NSMakePoint(NSMinX(card) + 8 * zoom, NSMidY(card) - 5 * zoom)];
            [marker lineToPoint:NSMakePoint(NSMinX(card) + 14 * zoom, NSMidY(card))];
            [marker lineToPoint:NSMakePoint(NSMinX(card) + 8 * zoom, NSMidY(card) + 5 * zoom)];
            [Paper() setFill];
            [marker fill];
        } else {
            Glass(card, radius, Paper(0.98), Mist(0.98), over ? Accent() : Line(0.8), 0.9);
        }
        NSString* place = Text(node[@"route"]).length ? Text(node[@"route"]) : Text(node[@"chapter"], @"剧情");
        Draw([NSString stringWithFormat:@"%@ · 选择 %02lu", place, (unsigned long)index],
             NSMakeRect(NSMinX(card) + 20 * zoom, NSMinY(card) + 7 * zoom, NSWidth(card) - 28 * zoom, 18 * zoom),
             Attributes(Round(12.5 * zoom, true), selected ? Paper() : Ink()));
        NSString* state = Boolean(node[@"can_restore"]) ? @"可以回到" : @"已到达";
        Draw([NSString stringWithFormat:@"%@ · %@ · 到达 %lld 次", state, DateText(node[@"last_visited"]), Count(node[@"visit_count"])],
             NSMakeRect(NSMinX(card) + 20 * zoom, NSMinY(card) + 27 * zoom, NSWidth(card) - 28 * zoom, 16 * zoom),
             Attributes(Round(9.6 * zoom), selected ? Paper(0.92) : AccentDeep()));
        AddHot(card, action, kGraph);
        NSArray* options = VisibleOptions(node);
        for (size_t i = 0; i < box->plates.size() && i < options.count; ++i) {
            NSDictionary* option = options[i];
            const NSRect plate = ToScreen(box->plates[i].second);
            const bool chosen = Boolean(option[@"chosen"]);
            // Card-to-option elbow.
            NSBezierPath* link = [NSBezierPath bezierPath];
            link.lineWidth = (chosen ? 1.5 : 1.0) * zoom;
            const CGFloat midY = NSMaxY(card) + 10 * zoom;
            [link moveToPoint:NSMakePoint(NSMidX(card), NSMaxY(card))];
            [link lineToPoint:NSMakePoint(NSMidX(card), midY)];
            [link lineToPoint:NSMakePoint(NSMidX(plate), midY)];
            [link lineToPoint:NSMakePoint(NSMidX(plate), NSMinY(plate))];
            [(chosen ? Accent(0.85) : Line(0.6)) setStroke];
            [link stroke];
            NSString* optionAction = [NSString stringWithFormat:@"option:%@:%lld", box->identifier, box->plates[i].first];
            const bool overOption = Hovered(optionAction);
            Fill(plate, 7 * zoom, chosen ? AccentSoft() : (overOption ? Mist() : Paper(0.94)));
            Stroke(plate, 7 * zoom, chosen ? Accent() : (overOption ? Accent(0.7) : LineSoft()));
            NSString* label = [NSString stringWithFormat:@"%lld. %@", Count(option[@"index"]), Text(option[@"text"])];
            Draw(label, NSMakeRect(NSMinX(plate) + 8 * zoom, NSMinY(plate) + 5 * zoom, NSWidth(plate) - 18 * zoom, NSHeight(plate) - 7 * zoom),
                 Attributes(Round(10.2 * zoom), chosen ? Ink() : (Boolean(option[@"available"]) ? InkSub() : InkFaint()),
                            NSTextAlignmentLeft, 0, false, 1));
            if (chosen) Draw(@"✓", NSMakeRect(NSMaxX(plate) - 13 * zoom, NSMaxY(plate) - 15 * zoom, 10 * zoom, 12 * zoom),
                             Attributes(Latin(9 * zoom, NSFontWeightBold), Accent()));
            AddHot(plate, optionAction, kGraph);
        }
    }
    [NSGraphicsContext restoreGraphicsState];
    AddHot(kGraph, @"board");  // empty board area: drag to pan
    // The board's hot area must stay behind the cards and plates.
    std::rotate(hots.begin(), hots.end() - 1, hots.end());
}

CGFloat DrawChapter(NSDictionary* chapter, CGFloat y) {
    NSString* identifier = Text(chapter[@"id"]);
    NSString* action = [@"select:chapter:" stringByAppendingString:identifier];
    const NSRect card = NSMakeRect(40, y, 484, 70);
    const bool selected = [selectedKind isEqual:@"chapter"] && [selectedID isEqual:identifier];
    if (selected) Glass(card, 11, AccentTop(), AccentBottom(), Paper(0.9), 0.7);
    else Glass(card, 11, Paper(0.98), Mist(0.98), Hovered(action) ? Accent() : Line(0.8), 0.9);
    const bool done = Completed(chapter);
    NSString* tag = done ? @"已完成" : @"进行中";
    NSDictionary* tagAttributes = Attributes(Round(9.5, true), done ? Color(26, 104, 78) : AccentDeep(), NSTextAlignmentCenter);
    const NSRect tagRect = NSMakeRect(56, y + 12, 46, 16);
    Fill(tagRect, 8, done ? Mint() : AccentSoft());
    Draw(tag, NSMakeRect(NSMinX(tagRect), NSMinY(tagRect) + 1.5, NSWidth(tagRect), 14), tagAttributes);
    Draw(Text(chapter[@"title"], @"章节"), NSMakeRect(112, y + 9, 300, 22), Attributes(Round(15, true), selected ? Paper() : Ink()));
    const long long total = Count(chapter[@"total"]), read = std::min(Count(chapter[@"read"]), total);
    const CGFloat ratio = total ? CGFloat(read) / total : 0;
    const NSRect track = NSMakeRect(56, y + 46, 300, 5);
    Fill(track, 2.5, selected ? Paper(0.35) : LineSoft());
    if (ratio > 0) Fill(NSMakeRect(NSMinX(track), NSMinY(track), std::max<CGFloat>(5, NSWidth(track) * ratio), 5), 2.5,
                        selected ? Paper() : Accent());
    if (total) Draw([NSString stringWithFormat:@"已读 %.1f%%  ·  %lld / %lld", ratio * 100, read, total],
                    NSMakeRect(366, y + 40, 150, 16), Attributes(Latin(9.5, NSFontWeightMedium), selected ? Paper() : InkSub()));
    AddHot(NSIntersectionRect(card, kList), action, kList);
    return y + 80;
}

CGFloat DrawEnding(NSDictionary* ending, CGFloat y) {
    NSString* identifier = Text(ending[@"id"]);
    NSString* action = [@"select:ending:" stringByAppendingString:identifier];
    const NSRect plate = NSMakeRect(74, y, 450, 30);
    const bool selected = [selectedKind isEqual:@"ending"] && [selectedID isEqual:identifier];
    Fill(plate, 8, selected ? AccentSoft() : (Hovered(action) ? Mist() : Paper(0.94)));
    Stroke(plate, 8, selected ? Accent() : LineSoft());
    Draw(@"✦", NSMakeRect(86, y + 6, 14, 18), Attributes(Latin(11, NSFontWeightBold), Accent()));
    Draw([NSString stringWithFormat:@"%@　已读至结局", Text(ending[@"title"], @"结局")], NSMakeRect(104, y + 6, 400, 20),
         Attributes(Round(12, true), Ink()));
    AddHot(plate, action, kList);
    return y + 38;
}

void DrawOverview() {
    CGFloat y = NSMinY(kList) + 6 - listScroll;
    NSUInteger shown = 0;
    [NSGraphicsContext saveGraphicsState];
    [[NSBezierPath bezierPathWithRect:kList] addClip];
    for (id value in Array(record[@"chapters"])) {
        NSDictionary* chapter = Dictionary(value);
        if (!Text(chapter[@"id"]).length || !Boolean(chapter[@"unlocked"])) continue;
        y = DrawChapter(chapter, y);
        ++shown;
        for (id endingValue in Array(record[@"endings"])) {
            NSDictionary* ending = Dictionary(endingValue);
            if (![Text(ending[@"chapter_id"]) isEqual:Text(chapter[@"id"])] || !Completed(ending)) continue;
            y = DrawEnding(ending, y);
            ++shown;
        }
        y += 6;
    }
    // A reached ending whose chapter catalogue is not available yet.
    for (id value in Array(record[@"endings"])) {
        NSDictionary* ending = Dictionary(value);
        bool listed = false;
        for (id chapter in Array(record[@"chapters"]))
            if (Boolean(Dictionary(chapter)[@"unlocked"]) && [Text(Dictionary(chapter)[@"id"]) isEqual:Text(ending[@"chapter_id"])]) listed = true;
        if (listed || !Completed(ending)) continue;
        y = DrawEnding(ending, y);
        ++shown;
    }
    [NSGraphicsContext restoreGraphicsState];
    listExtent = y + listScroll - NSMinY(kList);
    if (!shown) Draw(@"还没有到达的章节。\n推进剧情后，这里会逐渐出现内容。", NSMakeRect(60, 270, 444, 60),
                     Attributes(Round(13), InkSub(), NSTextAlignmentCenter, 0, false, 6));
}

void DrawSpecials() {
    CGFloat y = NSMinY(kList) + 6 - listScroll;
    NSString* section = nil;
    NSUInteger shown = 0, column = 0;
    [NSGraphicsContext saveGraphicsState];
    [[NSBezierPath bezierPathWithRect:kList] addClip];
    for (id value in Array(record[@"specials"])) {
        NSDictionary* special = Dictionary(value);
        if (!Text(special[@"id"]).length || !Boolean(special[@"unlocked"])) continue;
        NSString* current = Text(special[@"section"], @"已开放内容");
        if (![current isEqual:section]) {
            if (column) { y += 40; column = 0; }
            Fill(NSMakeRect(42, y + 4, 4, 15), 2, Accent());
            Draw(current, NSMakeRect(54, y + 1, 420, 20), Attributes(Round(13.5, true), Ink()));
            y += 28;
            section = current;
        }
        NSString* identifier = Text(special[@"id"]);
        NSString* action = [@"select:special:" stringByAppendingString:identifier];
        const NSRect plate = NSMakeRect(42 + column * 244, y, 236, 32);
        const bool selected = [selectedKind isEqual:@"special"] && [selectedID isEqual:identifier];
        if (selected) Glass(plate, 9, AccentTop(), AccentBottom(), Paper(0.9), 0.6);
        else Glass(plate, 9, Paper(0.98), Mist(0.98), Hovered(action) ? Accent() : Line(0.8), 0.9);
        Draw(Text(special[@"title"], @"特别内容"), NSMakeRect(NSMinX(plate) + 14, y + 7, 208, 20),
             Attributes(Round(12.5, true), selected ? Paper() : Ink()));
        AddHot(plate, action, kList);
        ++shown;
        if (++column == 2) { column = 0; y += 40; }
    }
    if (column) y += 40;
    [NSGraphicsContext restoreGraphicsState];
    listExtent = y + listScroll - NSMinY(kList);
    if (!shown) Draw(@"目前没有可显示的已开放内容。\n进入作品后，会按原作开放条件更新。", NSMakeRect(60, 270, 444, 60),
                     Attributes(Round(13), InkSub(), NSTextAlignmentCenter, 0, false, 6));
}

struct Detail {
    NSString* title = @"";
    NSString* state = @"";
    NSString* body = @"";
    NSString* action = nil;
    bool enabled = false;
    NSArray* checkpoints = @[];
};

Detail CurrentDetail() {
    Detail detail;
    NSDictionary* item = FindItem(selectedKind, selectedID);
    const bool pending = ActionPending() || awaiting;
    if (!item) {
        detail.title = page == 0 ? @"选择记录" : (page == 1 ? @"路线总览" : @"特别内容");
        detail.body = page == 0 ? @"选择左侧到达过的选择，可以查看当时出现的选项与已选标记。\n\n有可用记录时，可以回到那个选择，重新决定分支。" :
            @"这里只显示已经开放或实际到达的内容，尚未到达的章节、结局与内容不会显示。\n\n选择左侧项目查看详情。";
        return detail;
    }
    detail.title = Text(item[@"title"], @"已到达的选择");
    NSMutableString* body = [NSMutableString string];
    if ([selectedKind isEqual:@"node"]) {
        NSString* place = Text(item[@"route"]).length ? Text(item[@"route"]) : Text(item[@"chapter"]);
        if (place.length) detail.title = [place stringByAppendingString:@" · 选择"];
        NSArray* options = VisibleOptions(item);
        NSUInteger chosen = 0;
        for (NSDictionary* option in options) if (Boolean(option[@"chosen"])) ++chosen;
        [body appendFormat:@"已到达 %lld 次 · 已选 %lu / %lu 个选项\n", Count(item[@"visit_count"]),
            (unsigned long)chosen, (unsigned long)options.count];
        for (NSDictionary* option in options)
            [body appendFormat:@"\n%@ %lld. %@", Boolean(option[@"chosen"]) ? @"✓" : @"　", Count(option[@"index"]), Text(option[@"text"])];
        detail.checkpoints = Array(item[@"checkpoints"]);
        // Default to the newest usable record; keep an explicit valid choice.
        bool keep = false;
        for (id value in detail.checkpoints) {
            NSDictionary* checkpoint = Dictionary(value);
            if ([Text(checkpoint[@"id"]) isEqual:selectedCheckpoint] && Boolean(checkpoint[@"usable"])) keep = true;
        }
        if (!keep) {
            selectedCheckpoint = @"";
            for (id value in detail.checkpoints) if (Boolean(Dictionary(value)[@"usable"])) {
                selectedCheckpoint = Text(Dictionary(value)[@"id"]);
                break;
            }
        }
        detail.action = @"回到这个选择";
        detail.enabled = !pending && Boolean(item[@"can_restore"]) && selectedCheckpoint.length;
        detail.state = detail.enabled ? @"可以回到这里" : @"曾经到达";
        [body appendString:detail.enabled ? @"\n\n回到这个选择后，可以重新决定分支。" :
            @"\n\n目前没有可用的返回记录。再次在剧情中到达这个选择后会建立记录。"];
    } else {
        NSString* description = Text(item[@"description"]);
        if (description.length) [body appendString:description];
        const long long total = Count(item[@"total"]);
        if ([selectedKind isEqual:@"chapter"] && total > 0) {
            const long long read = std::min(Count(item[@"read"]), total);
            [body appendFormat:@"%@已读 %lld / %lld · %.1f%%", body.length ? @"\n\n" : @"", read, total, read * 100.0 / total];
        }
        detail.state = [selectedKind isEqual:@"ending"] ? @"已读至结局" :
            ([selectedKind isEqual:@"special"] ? @"已开放" : (Completed(item) ? @"已完成" : @"进行中"));
        if (![selectedKind isEqual:@"ending"]) {
            detail.action = [selectedKind isEqual:@"chapter"] ? @"从章节开头进入" : @"进入特别内容";
            detail.enabled = !pending && Boolean(item[@"enterable"]);
            if (!Boolean(item[@"enterable"]))
                [body appendString:[selectedKind isEqual:@"chapter"] ? @"\n\n还没有这一章开头的记录，从头游玩到这里后即可进入。" :
                    @"\n\n当前状态暂不能进入，请回到游戏标题后重试。"];
        }
        if ([selectedKind isEqual:@"special"] && [Text(item[@"section"]) containsString:@"音乐"])
            [body appendString:@"\n\n这里表示已开放、可以鉴赏的音乐，不代表已经收听的曲目数。"];
    }
    detail.body = body;
    return detail;
}

void DrawDetail() {
    Glass(kDetail, 14, Paper(0.9), Mist(0.92), Line(0.75), 0.95);
    const Detail detail = CurrentDetail();
    Fill(NSMakeRect(566, 108, 4, 20), 2, Accent());
    NSDictionary* titleAttributes = Attributes(Round(15, true), Ink());
    Draw(detail.title, NSMakeRect(578, 105, 186, 42), titleAttributes);
    const CGFloat titleHeight = std::min<CGFloat>(42, Measure(detail.title, 186, titleAttributes));
    CGFloat y = 112 + std::max<CGFloat>(20, titleHeight);
    Hairline(562, 766, y, Line(0.8));
    y += 12;
    if (detail.state.length) {
        Draw(detail.state, NSMakeRect(568, y, 196, 20), Attributes(Round(12.5, true), AccentDeep()));
        y += 24;
    }
    NSString* problem = localError.length ? localError : readError;
    NSDictionary* action = Dictionary(record[@"action"]);
    if (!problem.length && [Text(action[@"state"]) isEqual:@"failed"]) problem = Text(action[@"message"]);
    const CGFloat bottom = detail.checkpoints.count ? 434 : 466;
    Draw(detail.body, NSMakeRect(568, y, 196, bottom - y - (problem.length || statusNote.length ? 34 : 0)),
         Attributes(Round(11.2), InkSub(), NSTextAlignmentLeft, 0, false, 3));
    if (problem.length) Draw(problem, NSMakeRect(568, bottom - 32, 196, 32), Attributes(Round(10.5), Coral()));
    else if (statusNote.length) Draw(statusNote, NSMakeRect(568, bottom - 32, 196, 32), Attributes(Round(10.5), AccentDeep()));
    if (detail.checkpoints.count) {
        NSUInteger index = 0;
        for (NSUInteger i = 0; i < detail.checkpoints.count; ++i)
            if ([Text(Dictionary(detail.checkpoints[i])[@"id"]) isEqual:selectedCheckpoint]) index = i;
        NSDictionary* checkpoint = Dictionary(detail.checkpoints[index]);
        Hairline(562, 766, 440.5, Line(0.55));
        Button(NSMakeRect(564, 446, 26, 22), @"‹", @"checkpoint:-1", Style::Quiet, detail.checkpoints.count > 1);
        Button(NSMakeRect(738, 446, 26, 22), @"›", @"checkpoint:1", Style::Quiet, detail.checkpoints.count > 1);
        NSString* label = [NSString stringWithFormat:@"记录 %lu / %lu · %@%@", (unsigned long)index + 1,
            (unsigned long)detail.checkpoints.count, DateText(checkpoint[@"created_at"]), Boolean(checkpoint[@"usable"]) ? @"" : @"（不可用）"];
        Draw(label, NSMakeRect(590, 449, 148, 18), Attributes(Round(10.5), Ink(), NSTextAlignmentCenter));
    }
    if (detail.action) Button(NSMakeRect(566, 478, 196, 34), detail.action, @"navigate", Style::Primary, detail.enabled);
}

void DrawConfirm() {
    hots.clear();
    Fill(kScreen, 0, TealInk(0.28));
    const NSRect card = NSMakeRect(210, 204, 380, 176);
    Fill(NSOffsetRect(NSInsetRect(card, -2, -2), 0, 3), 18, TealInk(0.12));
    Glass(card, 16, Paper(), Mist(), Line(0.85), 1);
    const bool restore = [confirmKind isEqual:@"node"];
    Draw(restore ? @"回到这个选择？" : ([confirmKind isEqual:@"chapter"] ? @"从章节开头进入？" : @"进入特别内容？"),
         NSMakeRect(230, 226, 340, 24), Attributes(Round(16, true), Ink(), NSTextAlignmentCenter));
    Draw(restore ? @"将用这份记录替换当前的剧情状态，回到后可以重新选择分支。\n请先保存需要保留的进度。" :
                   @"这会离开当前的剧情并进入所选内容。\n请先保存需要保留的进度。",
         NSMakeRect(238, 262, 324, 60), Attributes(Round(11.5), InkSub(), NSTextAlignmentCenter, 0, false, 4));
    Button(NSMakeRect(250, 334, 140, 30), @"取消", @"confirm:no", Style::Secondary);
    Button(NSMakeRect(410, 334, 140, 30), restore ? @"回到并重新选择" : @"进入", @"confirm:yes", Style::Primary);
}

void DrawPage() {
    hots.clear();
    // The game's picture stays faintly visible behind the page.
    [[[NSGradient alloc] initWithStartingColor:Color(240, 248, 252, 0.95) endingColor:Color(214, 235, 246, 0.97)]
        drawInRect:kScreen angle:90];
    // Header: the English word large, the Chinese title beside it.
    NSDictionary* word = Attributes(Latin(20, NSFontWeightBold), Ink(), NSTextAlignmentLeft, 3.6);
    Draw(@"ROUTE MAP", NSMakeRect(28, 18, 200, 28), word);
    const CGFloat wordWidth = Width(@"ROUTE MAP", word);
    Draw(@"攻略进度", NSMakeRect(28 + wordWidth + 10, 25, 80, 18), Attributes(Round(12.5, true), AccentDeep()));
    Hairline(28 + wordWidth + 76, 610, 35.5, Line());
    const bool haruka = [game isEqual:@"haruka"];
    Button(NSMakeRect(626, 22, 72, 24), @"缘之空", @"game:yosuga", haruka ? Style::Tab : Style::TabActive);
    Button(NSMakeRect(704, 22, 72, 24), @"悠之空", @"game:haruka", haruka ? Style::TabActive : Style::Tab);
    NSArray* tabs = @[@"分支流程", @"路线总览", @"特别内容"];
    for (NSInteger i = 0; i < 3; ++i)
        Button(NSMakeRect(28 + i * 96, 54, 88, 26), tabs[i], [NSString stringWithFormat:@"tab:%ld", (long)i],
               i == page ? Style::TabActive : Style::Tab);
    NSString* stats = @"";
    if (summary) {
        NSDictionary* read = Dictionary(summary[@"read"]), *cg = Dictionary(summary[@"cg"]);
        NSDictionary* recollection = Dictionary(summary[@"recollection"]);
        const long long total = Count(read[@"total"]);
        if (total) stats = [NSString stringWithFormat:@"已读 %.1f%%　CG %lld/%lld　回想 %lld/%lld",
            std::min(Count(read[@"completed"]), total) * 100.0 / total, Count(cg[@"unlocked"]), Count(cg[@"total"]),
            Count(recollection[@"unlocked"]), Count(recollection[@"total"])];
    }
    if (!stats.length) stats = @"进入该作品后读取当前进度";
    Draw(stats, NSMakeRect(420, 60, 356, 16), Attributes(Round(10.5), InkSub(), NSTextAlignmentRight));
    Glass(kBoard, 14, Paper(0.9), Mist(0.92), Line(0.75), 0.95);
    if (page == 0) DrawFlow(); else if (page == 1) DrawOverview(); else DrawSpecials();
    DrawDetail();
    NSString* hint = page == 0 ? @"拖动平移 · 滚轮缩放 · 只连接实际走过的分支" :
        (page == 1 ? @"按原作已读记录显示路线进度，未到达的内容隐藏" : @"按原作开放条件显示");
    Draw(hint, NSMakeRect(28, 566, 520, 16), Attributes(Round(10.5), InkFaint()));
    Button(NSMakeRect(678, 560, 98, 28), @"返回", @"close", Style::Secondary, true, @"ESC");
    if (confirming) DrawConfirm();
}

// ---- actions
void Confirm() {
    if (!overlay) return;
    ParseRecord();
    if (page == 0) LayoutFlow();
    const Detail detail = CurrentDetail();
    if (!detail.action || !detail.enabled || confirming) return;
    confirming = true;
    confirmKind = [selectedKind copy];
    confirmID = [selectedID copy];
    confirmCheckpoint = [selectedCheckpoint copy];
    Redraw();
}

void Execute() {
    confirming = false;
    // Re-read: a removed or damaged record must not be used just because it
    // was selected a moment ago.
    ParseRecord();
    NSDictionary* current = FindItem(confirmKind, confirmID);
    const bool restore = [confirmKind isEqual:@"node"];
    bool usable = current && (restore ? Boolean(current[@"can_restore"]) : Boolean(current[@"enterable"]));
    if (restore) {
        bool found = false;
        for (id value in Array(current[@"checkpoints"])) {
            NSDictionary* checkpoint = Dictionary(value);
            if ([Text(checkpoint[@"id"]) isEqual:confirmCheckpoint] && Boolean(checkpoint[@"usable"])) found = true;
        }
        usable = usable && found;
    }
    if (!usable || ActionPending()) {
        localError = @"所选记录已不可用或内容尚未开放，当前剧情没有改变。";
        Redraw();
        return;
    }
    localError = @"";
    if (restore) entis::progress::RequestGuideRestore(game.UTF8String, confirmID.UTF8String, confirmCheckpoint.UTF8String);
    else entis::progress::RequestGuideEntry(game.UTF8String, confirmID.UTF8String);
    statusNote = restore ? @"正在校验记录并回到选择…" : @"正在准备进入…";
    awaiting = true;
    Redraw();
}

void Perform(NSString* action) {
    if ([action isEqual:@"close"]) { Close(); return; }
    if ([action hasPrefix:@"tab:"]) {
        page = [[action substringFromIndex:4] integerValue];
        Select(@"", @"");
        listScroll = 0;
        if (page == 0 && !placed) { Fit(); placed = true; }
    } else if ([action hasPrefix:@"game:"]) {
        NSString* next = [action substringFromIndex:5];
        if (![next isEqual:game]) {
            game = next;
            Select(@"", @"");
            listScroll = 0;
            placed = false;
            ParseRecord();
            LayoutFlow();
            Fit();
            placed = true;
        }
    } else if ([action hasPrefix:@"select:"]) {
        NSString* rest = [action substringFromIndex:7];
        const NSRange colon = [rest rangeOfString:@":"];
        if (colon.location != NSNotFound) Select([rest substringToIndex:colon.location], [rest substringFromIndex:colon.location + 1]);
    } else if ([action hasPrefix:@"option:"]) {
        NSString* rest = [action substringFromIndex:7];
        const NSRange colon = [rest rangeOfString:@":" options:NSBackwardsSearch];
        NSString* node = [rest substringToIndex:colon.location];
        const long long index = [[rest substringFromIndex:colon.location + 1] longLongValue];
        NSString* target = nil;
        for (NSDictionary* edge in edges)
            if ([Text(edge[@"source_id"]) isEqual:node] && Count(edge[@"option_index"]) == index) target = Text(edge[@"target_id"]);
        Select(@"node", target ?: node);
        if (target) Focus(target);
    } else if ([action isEqual:@"locate"] || [action isEqual:@"previous"] || [action isEqual:@"next"]) {
        if (!ordered.count) return;
        NSInteger current = -1;
        for (NSUInteger i = 0; i < ordered.count; ++i) if ([Text(ordered[i][@"id"]) isEqual:selectedID]) current = NSInteger(i);
        NSInteger target;
        if ([action isEqual:@"locate"]) {
            target = 0;
            for (NSUInteger i = 1; i < ordered.count; ++i)
                if (Count(ordered[i][@"last_visited"]) > Count(ordered[target][@"last_visited"])) target = NSInteger(i);
        } else {
            target = current < 0 ? 0 : current + ([action isEqual:@"next"] ? 1 : -1);
            target = std::clamp<NSInteger>(target, 0, NSInteger(ordered.count) - 1);
        }
        Select(@"node", Text(ordered[target][@"id"]));
        Focus(selectedID);
    } else if ([action hasPrefix:@"checkpoint:"]) {
        const Detail detail = CurrentDetail();
        const NSInteger step = [[action substringFromIndex:11] integerValue];
        NSInteger index = 0;
        for (NSUInteger i = 0; i < detail.checkpoints.count; ++i)
            if ([Text(Dictionary(detail.checkpoints[i])[@"id"]) isEqual:selectedCheckpoint]) index = NSInteger(i);
        for (NSUInteger tries = 0; tries < detail.checkpoints.count; ++tries) {
            index = (index + step + NSInteger(detail.checkpoints.count)) % NSInteger(detail.checkpoints.count);
            NSDictionary* checkpoint = Dictionary(detail.checkpoints[index]);
            if (Boolean(checkpoint[@"usable"])) { selectedCheckpoint = Text(checkpoint[@"id"]); break; }
        }
    } else if ([action isEqual:@"navigate"]) {
        Confirm();
        return;
    } else if ([action isEqual:@"confirm:yes"]) {
        Execute();
        return;
    } else if ([action isEqual:@"confirm:no"]) {
        confirming = false;
    }
    Redraw();
}

void Tick() {
    if (!overlay) return;
    const std::string json = entis::progress::LatestGuideJson(game.UTF8String);
    const std::string stats = entis::progress::LatestProgressJson(game.UTF8String);
    if (json == lastGuide && stats == lastSummary) return;
    ParseRecord();
    LayoutFlow();
    if (awaiting) {
        NSDictionary* action = Dictionary(record[@"action"]);
        NSString* state = Text(action[@"state"]);
        if ([state isEqual:@"completed"] || [state isEqual:@"restoring"]) {
            awaiting = false;
            statusNote = @"";
            Close();
            return;
        }
        if ([state isEqual:@"failed"]) {
            awaiting = false;
            statusNote = @"";
            localError = Text(action[@"message"], @"未能进入，当前剧情没有改变。");
        }
    }
    Redraw();
}

NSRect ContentRect(NSRect bounds) {
    const CGFloat scale = std::min(NSWidth(bounds) / 800, NSHeight(bounds) / 600);
    return NSMakeRect(NSMidX(bounds) - 400 * scale, NSMidY(bounds) - 300 * scale, 800 * scale, 600 * scale);
}
NSPoint Logical(NSView* view, NSEvent* event) {
    const NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
    const NSRect content = ContentRect(view.bounds);
    const CGFloat scale = NSWidth(content) / 800;
    return NSMakePoint((point.x - NSMinX(content)) / scale, (point.y - NSMinY(content)) / scale);
}
NSString* HotAt(NSPoint point) {
    for (auto at = hots.rbegin(); at != hots.rend(); ++at)
        if (NSPointInRect(point, at->rect)) return at->action;
    return nil;
}
} // namespace

void SetDataRoot(NSString* path) { dataRoot = [path copy]; }
bool Visible() { return overlay != nil; }

void Show(NSWindow* window, NSString* requested) {
    if (!window) return;
    host = window;
    NSString* wanted = requested ?: (Bridge::Get().ActiveGame() == Game::Haruka ? @"haruka" : @"yosuga");
    if (!overlay || ![wanted isEqual:game]) {
        if (![wanted isEqual:game]) { Select(@"", @""); placed = false; listScroll = 0; }
        game = wanted;
    }
    if (!overlay) {
        NSView* content = window.contentView;
        overlay = [[EntisGuideOverlay alloc] initWithFrame:content.bounds];
        overlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        overlay.wantsLayer = YES;
        overlay.alphaValue = 0;
        [content addSubview:overlay positioned:NSWindowAbove relativeTo:nil];
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
            context.duration = 0.18;
            overlay.animator.alphaValue = 1;
        }];
        confirming = awaiting = false;
        localError = statusNote = @"";
        timer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer*) { Tick(); }];
    }
    if (window.firstResponder != overlay) previousResponder = window.firstResponder;
    [window makeFirstResponder:overlay];
    ParseRecord();
    LayoutFlow();
    if (!placed) { Fit(); placed = true; }
    Redraw();
}

void Close() {
    if (!overlay) return;
    EntisGuideOverlay* closing = overlay;
    overlay = nil;
    confirming = false;
    hover = pressed = nil;
    panning = false;
    [timer invalidate];
    timer = nil;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
        context.duration = 0.14;
        closing.animator.alphaValue = 0;
    } completionHandler:^{
        [closing removeFromSuperview];
    }];
    if (host) [host makeFirstResponder:previousResponder ?: host.contentView];
}

void Refresh() { Tick(); }

bool HandleCommand(NSWindow* window, NSString* command) {
    if ([command isEqual:@"progress"] || [command isEqual:@"progress-yosuga"] || [command isEqual:@"progress-haruka"]) {
        Show(window, [command isEqual:@"progress"] ? nil : [command substringFromIndex:9]);
    } else if ([command isEqual:@"progress-flow"] || [command isEqual:@"progress-overview"] || [command isEqual:@"progress-special"]) {
        Show(window, nil);
        Perform([command isEqual:@"progress-overview"] ? @"tab:1" : ([command isEqual:@"progress-special"] ? @"tab:2" : @"tab:0"));
    } else if ([command hasPrefix:@"progress-select "]) {
        Show(window, nil);
        Perform(@"tab:0");
        NSString* identifier = [command substringFromIndex:16];
        if (FindItem(@"node", identifier)) { Select(@"node", identifier); Focus(identifier); Redraw(); }
    } else if ([command hasPrefix:@"progress-entry "]) {
        Show(window, nil);
        NSString* identifier = [command substringFromIndex:15];
        for (NSString* kind in @[@"chapter", @"special", @"ending"]) {
            if (!FindItem(kind, identifier)) continue;
            Perform([kind isEqual:@"special"] ? @"tab:2" : @"tab:1");
            Select(kind, identifier);
            Redraw();
            break;
        }
    } else if ([command hasPrefix:@"progress-checkpoint "]) {
        if (!DriverEnabled()) return true;
        NSString* identifier = [command substringFromIndex:20];
        for (id value in Array(FindItem(@"node", selectedID)[@"checkpoints"]))
            if ([Text(Dictionary(value)[@"id"]) isEqual:identifier] && Boolean(Dictionary(value)[@"usable"])) selectedCheckpoint = identifier;
        Redraw();
    } else if ([command hasPrefix:@"progress-dump "]) {
        NSString* target = [command substringFromIndex:14];
        if (!DriverEnabled() || !target.isAbsolutePath) return true;
        NSString* driver = [NSString stringWithUTF8String:SDL_getenv("ENTISGLS_DRIVER")];
        NSString* root = driver.stringByDeletingLastPathComponent.stringByResolvingSymlinksInPath;
        NSString* parent = target.stringByDeletingLastPathComponent.stringByResolvingSymlinksInPath;
        // Choice labels are private play history: exports stay beside the driver.
        if (![parent isEqual:root] && ![parent hasPrefix:[root stringByAppendingString:@"/"]]) return true;
        const std::string json = entis::progress::LatestGuideJson(game.UTF8String);
        [[NSData dataWithBytes:json.data() length:json.size()] writeToFile:target atomically:YES];
    } else if ([command isEqual:@"progress-restore"] || [command isEqual:@"progress-enter"]) {
        Confirm();
    } else if ([command isEqual:@"progress-confirm"]) {
        if (DriverEnabled(true) && confirming) Execute();
    } else if ([command isEqual:@"progress-cancel-confirm"]) {
        if (confirming) Perform(@"confirm:no");
    } else if ([command hasPrefix:@"progress-zoom "]) {
        if (!DriverEnabled()) return true;
        const double scale = [[command substringFromIndex:14] doubleValue];
        if (std::isfinite(scale)) { zoom = std::clamp(scale, 0.45, 1.6); ClampPan(); Redraw(); }
    } else if ([command isEqual:@"progress-fit"]) {
        if (DriverEnabled()) { Fit(); Redraw(); }
    } else if ([command hasPrefix:@"progress-click "] || [command hasPrefix:@"progress-hover "]) {
        // Driver clicks are SDL events and never reach this AppKit page; the
        // isolated test driver presses the page's own targets instead.
        double x = 0, y = 0;
        if (!overlay || !DriverEnabled(true) || std::sscanf([command substringFromIndex:15].UTF8String, "%lf %lf", &x, &y) != 2)
            return true;
        NSString* at = HotAt(NSMakePoint(x, y));
        if ([command hasPrefix:@"progress-hover "]) { hover = [at isEqual:@"board"] ? nil : at; Redraw(); }
        else if (at && ![at isEqual:@"board"]) Perform(at);
    } else if ([command isEqual:@"close-progress"]) {
        Close();
    } else {
        return false;
    }
    return true;
}

} // namespace entis::mac::guide

using namespace entis::mac::guide;

@implementation EntisGuideOverlay
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (void)updateTrackingAreas {
    for (NSTrackingArea* area in self.trackingAreas) [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:self userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)drawRect:(NSRect)dirty {
    const NSRect content = ContentRect(self.bounds);
    const CGFloat scale = NSWidth(content) / 800;
    [NSGraphicsContext saveGraphicsState];
    NSAffineTransform* transform = [NSAffineTransform transform];
    [transform translateXBy:NSMinX(content) yBy:NSMinY(content)];
    [transform scaleBy:scale];
    [transform concat];
    [[NSBezierPath bezierPathWithRect:kScreen] addClip];
    DrawPage();
    [NSGraphicsContext restoreGraphicsState];
}
- (void)mouseMoved:(NSEvent*)event {
    NSString* at = HotAt(Logical(self, event));
    if ([at isEqual:@"board"]) at = nil;
    if (at != hover && ![at isEqual:hover]) { hover = at; self.needsDisplay = YES; }
    [((at && ![at isEqual:@"board"]) ? NSCursor.pointingHandCursor : NSCursor.arrowCursor) set];
}
- (void)mouseDown:(NSEvent*)event {
    const NSPoint point = Logical(self, event);
    pressed = HotAt(point);
    if ([pressed isEqual:@"board"] && page == 0 && !confirming) {
        panning = true;
        panStart = point;
        panOrigin = pan;
        pressed = nil;
        [NSCursor.closedHandCursor set];
    }
}
- (void)mouseDragged:(NSEvent*)event {
    if (!panning) return;
    const NSPoint point = Logical(self, event);
    pan = CGPointMake(panOrigin.x + point.x - panStart.x, panOrigin.y + point.y - panStart.y);
    ClampPan();
    self.needsDisplay = YES;
}
- (void)mouseUp:(NSEvent*)event {
    if (panning) { panning = false; [NSCursor.arrowCursor set]; return; }
    NSString* at = HotAt(Logical(self, event));
    NSString* was = pressed;
    pressed = nil;
    if (was && [was isEqual:at] && ![at isEqual:@"board"]) Perform(at);
}
- (void)rightMouseDown:(NSEvent*)event {
    if (confirming) Perform(@"confirm:no"); else Close();
}
- (void)scrollWheel:(NSEvent*)event {
    if (confirming) return;
    const NSPoint point = Logical(self, event);
    if (page == 0 && NSPointInRect(point, kGraph)) {
        if (event.hasPreciseScrollingDeltas) {
            pan = CGPointMake(pan.x + event.scrollingDeltaX * 0.5, pan.y + event.scrollingDeltaY * 0.5);
        } else {
            const CGFloat previous = zoom;
            zoom = std::clamp<CGFloat>(zoom * (event.scrollingDeltaY > 0 ? 1.08 : 1 / 1.08), 0.45, 1.6);
            const CGFloat x = point.x - NSMinX(kGraph), y = point.y - NSMinY(kGraph);
            pan = CGPointMake(x - (x - pan.x) * zoom / previous, y - (y - pan.y) * zoom / previous);
        }
        ClampPan();
    } else if (page != 0 && NSPointInRect(point, kList)) {
        const CGFloat delta = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY * 0.5 : event.scrollingDeltaY * 24;
        listScroll = std::clamp<CGFloat>(listScroll - delta, 0, std::max<CGFloat>(0, listExtent - NSHeight(kList) + 12));
    }
    self.needsDisplay = YES;
}
- (void)magnifyWithEvent:(NSEvent*)event {
    if (page != 0 || confirming) return;
    zoom = std::clamp<CGFloat>(zoom * (1 + event.magnification), 0.45, 1.6);
    ClampPan();
    self.needsDisplay = YES;
}
- (void)keyDown:(NSEvent*)event {
    NSString* key = event.charactersIgnoringModifiers;
    const unichar code = key.length ? [key characterAtIndex:0] : 0;
    if (code == 27) { if (confirming) Perform(@"confirm:no"); else Close(); return; }
    if (confirming) {
        if (code == '\r' || code == 3) Perform(@"confirm:yes");
        return;
    }
    if (code == NSLeftArrowFunctionKey) Perform([NSString stringWithFormat:@"tab:%ld", (long)std::max<NSInteger>(0, page - 1)]);
    else if (code == NSRightArrowFunctionKey) Perform([NSString stringWithFormat:@"tab:%ld", (long)std::min<NSInteger>(2, page + 1)]);
    else if (page == 0 && code == NSUpArrowFunctionKey) Perform(@"previous");
    else if (page == 0 && code == NSDownArrowFunctionKey) Perform(@"next");
    else if (code == '\r' || code == 3) Perform(@"navigate");
}
@end
