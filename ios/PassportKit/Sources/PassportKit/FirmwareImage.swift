// Inspects a firmware file the way the resident installer will see it.
// The table rules mirror components/installer_core/inst_table.c so the app can
// explain incompatibility before connecting; the device still decides.
import CryptoKit
import Foundation

public struct Partition: Equatable {
    public var type: UInt8
    public var subtype: UInt8
    public var offset: UInt32
    public var size: UInt32
    public var label: String
    public var flags: UInt32
    public var end: UInt32 { offset + size }
}

public enum FirmwareKind: String, Codable {
    /// `idf.py merge-bin` output, written from 0x0.
    case merged
    /// Application image only (for example build/FoloToy-AI-Passport.bin). The
    /// app wraps it in the repository's default partition layout.
    case applicationOnly
}

public enum FirmwareImageError: Error, Equatable {
    case tooSmall
    case notESP32C3
    case unrecognized

    public var message: String {
        switch self {
        case .tooSmall: return "文件太小，不是完整固件"
        case .notESP32C3: return "这个固件不是为 ESP32-C3 编译的"
        case .unrecognized: return "无法识别：既不是合并固件，也不是应用镜像"
        }
    }
}

public struct FirmwareImage {
    /// Bytes as the device sees them from 0x0 (wrapped for application-only files).
    public let bytes: Data
    public let kind: FirmwareKind
    /// End of meaningful data: trailing 0xFF removed.
    public let dataEnd: UInt32
    public let partitions: [Partition]
    /// nil when the installer will accept the table; otherwise the reason.
    public let tableError: TableError?
    /// SHA-256 of bytes [0x8000, dataEnd): what the device verifies after writing.
    public let streamSHA256: Data

    public var compatible: Bool { tableError == nil }

    public init(fileData: Data) throws {
        let raw = [UInt8](fileData)
        guard raw.count >= 24 else { throw FirmwareImageError.tooSmall }
        if raw.count > Int(InstallerLayout.tableOffset) + 32,
           raw[Int(InstallerLayout.tableOffset)] == 0xAA,
           raw[Int(InstallerLayout.tableOffset) + 1] == 0x50 {
            guard raw[0] == 0xE9 else { throw FirmwareImageError.unrecognized }
            kind = .merged
            bytes = fileData
        } else if raw[0] == 0xE9 {
            guard raw.le16(12) == 5 else { throw FirmwareImageError.notESP32C3 }
            kind = .applicationOnly
            bytes = FirmwareImage.wrapApplication(fileData)
        } else {
            throw FirmwareImageError.unrecognized
        }
        let all = [UInt8](bytes)
        var end = all.count
        while end > 0 && all[end - 1] == 0xFF { end -= 1 }
        dataEnd = UInt32(end)
        guard end > Int(InstallerLayout.playStart) else { throw FirmwareImageError.tooSmall }
        let tableBytes = Array(all[Int(InstallerLayout.tableOffset)..<Int(InstallerLayout.tableOffset) + InstallerLayout.tableMax])
        let parsed = FirmwareImage.parseTable(tableBytes)
        partitions = parsed.partitions
        tableError = FirmwareImage.check(parsed, dataEnd: dataEnd)
        streamSHA256 = Data(SHA256.hash(data: bytes[Int(InstallerLayout.streamStart)..<end]))
    }

    /// Bytes to send: [0x8000, dataEnd).
    public var stream: Data { bytes[Int(InstallerLayout.streamStart)..<Int(dataEnd)] }

    /// Size the application partition will have on the device.
    public var bootPartition: Partition? {
        partitions.first { $0.type == 0 && $0.subtype == 0x00 }
            ?? partitions.first { $0.type == 0 && $0.subtype == 0x10 }
    }

    // MARK: - Partition table

    struct ParsedTable {
        var partitions: [Partition] = []
        var error: TableError?
    }

    static func parseTable(_ t: [UInt8]) -> ParsedTable {
        var result = ParsedTable()
        var at = 0
        while at + 32 <= t.count {
            if t[at] == 0xEB && t[at + 1] == 0xEB {
                let digest = Data(Insecure.MD5.hash(data: Data(t[0..<at])))
                if digest != Data(t[(at + 16)..<(at + 32)]) { result.error = .format }
                return result
            }
            if t[at] == 0xFF && t[at + 1] == 0xFF { break }
            guard t[at] == 0xAA && t[at + 1] == 0x50 else {
                result.error = .format
                return result
            }
            if result.partitions.count == InstallerLayout.tableMax / 32 - 1 {
                result.error = .full
                return result
            }
            let labelBytes = t[(at + 12)..<(at + 28)]
            guard labelBytes.contains(0) else {
                result.error = .entry
                return result
            }
            let label = String(decoding: labelBytes.prefix { $0 != 0 }, as: UTF8.self)
            result.partitions.append(Partition(type: t[at + 2], subtype: t[at + 3],
                                               offset: t.le32(at + 4), size: t.le32(at + 8),
                                               label: label, flags: t.le32(at + 28)))
            at += 32
        }
        return result
    }

    static func check(_ parsed: ParsedTable, dataEnd: UInt32) -> TableError? {
        if let error = parsed.error { return error }
        let limit = InstallerLayout.playLimit
        if dataEnd > limit { return .imageEnd }
        let parts = parsed.partitions
        if parts.isEmpty { return .format }
        for p in parts {
            if p.label.isEmpty || p.flags & 1 != 0 || p.offset < InstallerLayout.playStart ||
                p.offset % 0x1000 != 0 || p.size == 0 || p.size % 0x1000 != 0 ||
                UInt64(p.offset) + UInt64(p.size) > UInt64(InstallerLayout.flashSize) ||
                (p.type == 0 && p.offset % 0x10000 != 0) {
                return .entry
            }
        }
        for i in parts.indices {
            for j in 0..<i where parts[i].label == parts[j].label ||
                (parts[i].offset < parts[j].end && parts[j].offset < parts[i].end) {
                return .overlap
            }
        }
        var bootOffset: UInt32?
        var ota0: UInt32?
        var kept = 0
        for p in parts {
            if isInstallerEntry(p) { continue }
            if Self.reservedLabels.contains(p.label) { return .reserved }
            if p.type == 0 && p.subtype == 0x20 { return .reserved }
            if p.end > limit {
                if p.offset >= limit { return .reserved }
                if p.type != 0 && dataEnd > p.offset { return .reserved }
            }
            if p.type == 0 && p.subtype == 0 { bootOffset = p.offset }
            if p.type == 0 && p.subtype == 0x10 && ota0 == nil { ota0 = p.offset }
            kept += 1
        }
        guard let boot = bootOffset ?? ota0, dataEnd > boot else { return .noApp }
        if kept + 3 > InstallerLayout.tableMax / 32 - 1 { return .full }
        return nil
    }

    static let reservedLabels: Set<String> = ["inst_state", "installer", "inst_wifi"]

    /// Installer entries from any template revision; the device re-appends
    /// them in canonical form.
    static func isInstallerEntry(_ p: Partition) -> Bool {
        reservedLabels.contains(p.label) && p.offset >= InstallerLayout.playLimit
    }

    // MARK: - Application-only images

    /// Default layout of this repository: nvs, phy_init, factory up to the installer.
    static func defaultTable() -> Data {
        var table = Data()
        func entry(_ type: UInt8, _ subtype: UInt8, _ offset: UInt32, _ size: UInt32, _ label: String) {
            var e = Data([0xAA, 0x50, type, subtype])
            e.appendLE(offset)
            e.appendLE(size)
            var name = Data(label.utf8)
            name.append(Data(repeating: 0, count: 16 - name.count))
            e.append(name)
            e.appendLE(0)
            table.append(e)
        }
        entry(1, 0x02, 0x9000, 0x6000, "nvs")
        entry(1, 0x01, 0xF000, 0x1000, "phy_init")
        entry(0, 0x00, 0x10000, InstallerLayout.playLimit - 0x10000, "factory")
        var md5 = Data([0xEB, 0xEB])
        md5.append(Data(repeating: 0xFF, count: 14))
        md5.append(Data(Insecure.MD5.hash(data: table)))
        table.append(md5)
        return table
    }

    static func wrapApplication(_ app: Data) -> Data {
        var image = Data(repeating: 0xFF, count: 0x10000)
        let table = defaultTable()
        image.replaceSubrange(Int(InstallerLayout.tableOffset)..<Int(InstallerLayout.tableOffset) + table.count, with: table)
        image.append(app)
        return image
    }
}

public extension Data {
    var hexString: String { map { String(format: "%02x", $0) }.joined() }
}

extension Array where Element == UInt8 {
    func le16(_ at: Int) -> UInt16 { UInt16(self[at]) | UInt16(self[at + 1]) << 8 }
}
