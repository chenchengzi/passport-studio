// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "PassportKit",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [
        .library(name: "PassportKit", targets: ["PassportKit"]),
    ],
    targets: [
        .target(name: "PassportKit"),
        .testTarget(name: "PassportKitTests", dependencies: ["PassportKit"]),
    ]
)
