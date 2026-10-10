import AppKit
import Foundation

let root = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : FileManager.default.currentDirectoryPath)
let assets = root.appendingPathComponent("assets")
let iconset = assets.appendingPathComponent("Across.iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

func render(size: Int, destination: URL) throws {
    let colorSpace = CGColorSpaceCreateDeviceRGB()
    guard let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: size * 4, space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { fatalError("Cannot render icon") }
    let scale = CGFloat(size) / 512
    context.scaleBy(x: scale, y: scale)
    context.translateBy(x: 0, y: 512)
    context.scaleBy(x: 1, y: -1)
    context.setFillColor(CGColor(red: 12/255, green: 18/255, blue: 28/255, alpha: 1))
    context.addPath(CGPath(roundedRect: CGRect(x: 24, y: 24, width: 464, height: 464), cornerWidth: 104, cornerHeight: 104, transform: nil))
    context.fillPath()
    context.setLineWidth(32)
    context.setLineCap(.round)
    context.setStrokeColor(CGColor(red: 235/255, green: 241/255, blue: 249/255, alpha: 1))
    context.strokeEllipse(in: CGRect(x: 118, y: 194, width: 124, height: 124))
    context.move(to: CGPoint(x: 242, y: 194))
    context.addLine(to: CGPoint(x: 242, y: 318))
    context.strokePath()
    context.setStrokeColor(CGColor(red: 83/255, green: 137/255, blue: 1, alpha: 1))
    context.move(to: CGPoint(x: 298, y: 194))
    context.addLine(to: CGPoint(x: 398, y: 318))
    context.move(to: CGPoint(x: 398, y: 194))
    context.addLine(to: CGPoint(x: 298, y: 318))
    context.strokePath()
    guard let image = context.makeImage() else { fatalError("Cannot create icon image") }
    let bitmap = NSBitmapImageRep(cgImage: image)
    guard let png = bitmap.representation(using: .png, properties: [:]) else { fatalError("Cannot encode icon") }
    try png.write(to: destination)
}

try render(size: 1024, destination: assets.appendingPathComponent("across-icon.png"))
for size in [16, 32, 128, 256, 512] {
    try render(size: size, destination: iconset.appendingPathComponent("icon_\(size)x\(size).png"))
    try render(size: size * 2, destination: iconset.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", iconset.path, "-o", assets.appendingPathComponent("Across.icns").path]
try process.run()
process.waitUntilExit()
guard process.terminationStatus == 0 else { fatalError("Cannot create macOS icon") }
try FileManager.default.removeItem(at: iconset)
