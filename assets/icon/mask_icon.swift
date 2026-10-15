// Clips the rendered 1024 px icon to the macOS icon grid: a 824 px rounded square at a 100 px margin,
// corner radius 185. Everything outside the square becomes transparent.
// Usage: swift mask_icon.swift IN.png OUT.png
import Foundation
import CoreGraphics
import ImageIO

let args = CommandLine.arguments
guard args.count == 3 else {
    fputs("usage: mask_icon.swift IN.png OUT.png\n", stderr)
    exit(2)
}
guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil) else {
    fputs("cannot open \(args[1])\n", stderr)
    exit(1)
}
guard let image = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    fputs("cannot decode \(args[1])\n", stderr)
    exit(1)
}
let side = 1024
let space = CGColorSpace(name: CGColorSpace.sRGB)!
guard let ctx = CGContext(data: nil, width: side, height: side, bitsPerComponent: 8, bytesPerRow: 0,
                          space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
    fputs("no context\n", stderr)
    exit(1)
}
ctx.clear(CGRect(x: 0, y: 0, width: side, height: side))
let square = CGRect(x: 100, y: 100, width: 824, height: 824)
ctx.addPath(CGPath(roundedRect: square, cornerWidth: 185, cornerHeight: 185, transform: nil))
ctx.clip()
ctx.draw(image, in: CGRect(x: 0, y: 0, width: side, height: side))
guard let out = ctx.makeImage(),
      let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: args[2]) as CFURL, "public.png" as CFString, 1, nil) else {
    fputs("cannot write output\n", stderr)
    exit(1)
}
CGImageDestinationAddImage(dest, out, nil)
CGImageDestinationFinalize(dest)
print("wrote \(args[2])")
