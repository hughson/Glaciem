// make_icon.swift -- draws the Glaciem Miner app icon (1024x1024 PNG).
// Dark rounded square with a glowing glacier-cyan crystal-lattice mark: a
// hexagonal unit cell (centre node, six ring nodes, edges and spokes)
// with outward stubs suggesting the lattice repeating -- evokes Glaciem's
// ice-crystal theme and the "Lattice" proof-of-work.
// Run: swift make_icon.swift   ->   icon_1024.png

import AppKit
import Foundation

let S: CGFloat = 1024
guard let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: Int(S), pixelsHigh: Int(S),
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else {
    fputs("bitmap rep failed\n", stderr); exit(1)
}

NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!

let amber = NSColor(red: 0.247, green: 0.757, blue: 0.878, alpha: 1)

// rounded-square icon shape, with transparent margin (macOS convention)
let m: CGFloat = 86
let rect = NSRect(x: m, y: m, width: S - 2*m, height: S - 2*m)
let shape = NSBezierPath(roundedRect: rect, xRadius: 206, yRadius: 206)

// --- background ---
ctx.saveGraphicsState()
shape.addClip()
NSGradient(colors: [NSColor(red:0.13, green:0.13, blue:0.15, alpha:1),
                    NSColor(red:0.045,green:0.045,blue:0.055,alpha:1)])!
    .draw(in: rect, angle: 90)
// soft amber glow centred behind the lattice
NSGradient(colors: [NSColor(red:0.247, green:0.757, blue:0.878, alpha:0.42),
                    NSColor(red:0.247, green:0.757, blue:0.878, alpha:0.0)])!
    .draw(in: shape, relativeCenterPosition: NSPoint(x: 0, y: 0))
ctx.restoreGraphicsState()

// --- crystal-lattice mark: a hexagonal unit cell ---
let cx = S/2, cy = S/2
let center = NSPoint(x: cx, y: cy)
let R: CGFloat       = 270   // circumradius of the hexagon ring
let stub: CGFloat    = 110   // outward stub length (lattice continuing)
let edgeW: CGFloat   = 24
let nodeR: CGFloat   = 32
let centerR: CGFloat = 44
let tipR: CGFloat    = 22

// pointy-top hexagon: a vertex every 60 deg, first one straight up
func vert(_ k: Int, _ radius: CGFloat) -> NSPoint {
    let a = Double(90 + k*60) * Double.pi / 180.0
    return NSPoint(x: cx + radius*CGFloat(cos(a)), y: cy + radius*CGFloat(sin(a)))
}
let hexV = (0..<6).map { vert($0, R) }
let tipV = (0..<6).map { vert($0, R + stub) }

ctx.saveGraphicsState()
let glow = NSShadow()
glow.shadowColor = NSColor(red:0.247, green:0.757, blue:0.878, alpha:0.95)
glow.shadowBlurRadius = 46
glow.shadowOffset = .zero
glow.set()

// edges
amber.setStroke()
let edges = NSBezierPath()
edges.lineWidth = edgeW
edges.lineCapStyle = .round
edges.lineJoinStyle = .round
for k in 0..<6 {                                   // hexagon perimeter
    edges.move(to: hexV[k]); edges.line(to: hexV[(k+1)%6])
}
for k in 0..<6 {                                   // spokes from centre
    edges.move(to: center); edges.line(to: hexV[k])
}
for k in 0..<6 {                                   // outward stubs
    edges.move(to: hexV[k]); edges.line(to: tipV[k])
}
edges.stroke()

// nodes
amber.setFill()
func dot(_ p: NSPoint, _ r: CGFloat) {
    NSBezierPath(ovalIn: NSRect(x: p.x-r, y: p.y-r, width: 2*r, height: 2*r)).fill()
}
for p in hexV { dot(p, nodeR) }
for p in tipV { dot(p, tipR) }
dot(center, centerR)
ctx.restoreGraphicsState()

NSGraphicsContext.restoreGraphicsState()

guard let png = rep.representation(using: .png, properties: [:]) else {
    fputs("png encode failed\n", stderr); exit(1)
}
try! png.write(to: URL(fileURLWithPath: "icon_1024.png"))
print("icon_1024.png written")
