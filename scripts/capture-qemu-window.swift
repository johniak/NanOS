// capture-qemu-window.swift — capture a window owned by a given app to a PNG, even when the
// window is occluded (e.g. a fullscreen video is on top). Uses CGWindowListCreateImage with the
// specific window ID, which composites that window's own backing store.
// Usage: swift capture-qemu-window.swift <owner-substring> <out.png>
import Cocoa
import CoreGraphics

let owner = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "qemu"
let outPath = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : "/tmp/qwin.png"

guard let infos = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] else {
    FileHandle.standardError.write("no window list\n".data(using: .utf8)!); exit(2)
}
var best: (id: CGWindowID, area: CGFloat)? = nil
for w in infos {
    guard let name = w[kCGWindowOwnerName as String] as? String,
          name.lowercased().contains(owner.lowercased()),
          let wid = w[kCGWindowNumber as String] as? CGWindowID,
          let b = w[kCGWindowBounds as String] as? [String: Any],
          let wd = b["Width"] as? CGFloat, let ht = b["Height"] as? CGFloat else { continue }
    let area = wd * ht
    if best == nil || area > best!.area { best = (wid, area) }
}
guard let pick = best else {
    FileHandle.standardError.write("no window for owner \(owner)\n".data(using: .utf8)!); exit(3)
}
let opts: CGWindowImageOption = [.boundsIgnoreFraming, .bestResolution]
guard let img = CGWindowListCreateImage(.null, .optionIncludingWindow, pick.id, opts) else {
    FileHandle.standardError.write("capture failed\n".data(using: .utf8)!); exit(4)
}
let rep = NSBitmapImageRep(cgImage: img)
guard let png = rep.representation(using: .png, properties: [:]) else {
    FileHandle.standardError.write("png encode failed\n".data(using: .utf8)!); exit(5)
}
try? png.write(to: URL(fileURLWithPath: outPath))
print("captured window id=\(pick.id) -> \(outPath) (\(img.width)x\(img.height))")
