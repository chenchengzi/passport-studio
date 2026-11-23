import PassportKit
import SwiftUI

enum Format {
    static func bytes(_ count: Int) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(count), countStyle: .binary)
    }

    static func bytes(_ count: UInt32) -> String { bytes(Int(count)) }

    static func date(_ date: Date) -> String {
        date.formatted(date: .abbreviated, time: .shortened)
    }

    static func duration(_ seconds: Double) -> String {
        guard seconds.isFinite, seconds > 0 else { return "--" }
        let s = Int(seconds.rounded())
        return s >= 60 ? "\(s / 60) 分 \(s % 60) 秒" : "\(s) 秒"
    }

    static func shortHash(_ hex: String) -> String {
        hex.count > 16 ? "\(hex.prefix(8))…\(hex.suffix(8))" : hex
    }
}

struct StatusBadge: View {
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .foregroundStyle(color)
            .background(color.opacity(0.15), in: Capsule())
    }
}

extension FirmwareItem {
    var sourceSymbol: String { isCommunity ? "globe.asia.australia" : "hammer" }

    var sourceDescription: String {
        switch source {
        case .local(let fileName): return "本地文件 \(fileName)"
        case .community(let slug, _, let author, _):
            return author.map { "社区玩法 · \($0)" } ?? "社区玩法 \(slug)"
        }
    }
}
