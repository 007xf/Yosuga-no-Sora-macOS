// Ensures the interface fonts exist: 圆体-简 (Yuanti SC) is an Apple
// downloadable font, not installed by default. CoreText fetches it from
// Apple's font asset server into /System/Library/AssetsV2 (no admin needed).
//
//     swift tools/fonts.swift        # prints the font files, downloads if missing
import CoreText
import Foundation

let names = ["STYuanti-SC-Regular", "STYuanti-SC-Bold", "STYuanti-SC-Light"]
let descriptors = names.map {
    CTFontDescriptorCreateWithAttributes([kCTFontNameAttribute: $0] as CFDictionary)
} as CFArray

func path(_ name: String) -> String? {
    let font = CTFontCreateWithName(name as CFString, 12, nil)
    guard CTFontCopyPostScriptName(font) as String == name,
          let url = CTFontCopyAttribute(font, kCTFontURLAttribute) as? URL else { return nil }
    return url.path
}

if names.allSatisfy({ path($0) != nil }) {
    names.forEach { print(path($0)!) }
    exit(0)
}

let done = DispatchSemaphore(value: 0)
var failed = false
CTFontDescriptorMatchFontDescriptorsWithProgressHandler(descriptors, nil) { state, progress in
    switch state {
    case .didFailWithError:
        failed = true
        FileHandle.standardError.write("font download failed: \(progress)\n".data(using: .utf8)!)
    case .didFinish:
        done.signal()
    default:
        break
    }
    return true
}
done.wait()
let missing = names.filter { path($0) == nil }
if failed || !missing.isEmpty {
    FileHandle.standardError.write("missing fonts: \(missing)\n".data(using: .utf8)!)
    exit(1)
}
names.forEach { print(path($0)!) }
