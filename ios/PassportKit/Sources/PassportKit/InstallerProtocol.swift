// Mirror of components/installer_core/include/inst_proto.h and inst_layout.h.
// Keep both sides in sync; the device firmware is the authority.
import Foundation

public enum InstallerLayout {
    public static let flashSize: UInt32 = 0x800000
    public static let tableOffset: UInt32 = 0x8000
    public static let tableMax = 0xC00
    public static let streamStart: UInt32 = 0x8000
    public static let playStart: UInt32 = 0x9000
    public static let playLimit: UInt32 = 0x6FF000
    public static let stateOffset: UInt32 = 0x6FF000
    public static let stateSize: UInt32 = 0x1000
    public static let appOffset: UInt32 = 0x700000
    public static let appSize: UInt32 = 0xFF000
    public static let wifiOffset: UInt32 = 0x7FF000
    public static let wifiSize: UInt32 = 0x1000
    public static let sector = 0x1000
    public static let nameMax = 63
}

public enum InstallerUUID {
    public static let service = "CDDE6C00-DD5A-47C6-890F-268DE0F6918A"
    public static let info = "CDDE6C01-DD5A-47C6-890F-268DE0F6918A"
    public static let control = "CDDE6C02-DD5A-47C6-890F-268DE0F6918A"
    public static let data = "CDDE6C03-DD5A-47C6-890F-268DE0F6918A"
    public static let status = "CDDE6C04-DD5A-47C6-890F-268DE0F6918A"
    public static let wifi = "CDDE6C05-DD5A-47C6-890F-268DE0F6918A"
}

public enum InstallerOp: UInt8 {
    case begin = 0x01
    case commit = 0x02
    case abort = 0x03
    case reboot = 0x04
    case data = 0x10
    case skip = 0x11
    case wifiSet = 0x20
}

public enum InstallerProtocol {
    public static let version: UInt8 = 2
    /// First protocol version with the WIFI characteristic and WIFI_SET.
    public static let wifiVersion: UInt8 = 2
    public static let dataHeader = 5
    public static let windowBytes = 16 * 1024
    public static let ackInterval = 4096

    public static func begin(end: UInt32, streamSHA256: Data, name: String) -> Data {
        precondition(streamSHA256.count == 32)
        let nameBytes = truncatedUTF8(name, maxBytes: InstallerLayout.nameMax)
        var packet = Data([InstallerOp.begin.rawValue])
        packet.appendLE(end)
        packet.append(streamSHA256)
        packet.append(UInt8(nameBytes.count))
        packet.append(nameBytes)
        return packet
    }

    public static func command(_ op: InstallerOp) -> Data {
        Data([op.rawValue])
    }

    public static func data(offset: UInt32, payload: Data) -> Data {
        var packet = Data([InstallerOp.data.rawValue])
        packet.appendLE(offset)
        packet.append(payload)
        return packet
    }

    public static func skip(offset: UInt32, to: UInt32) -> Data {
        var packet = Data([InstallerOp.skip.rawValue])
        packet.appendLE(offset)
        packet.appendLE(to)
        return packet
    }

    /// Replace the device's saved networks (priority order). Validate first.
    public static func wifiSet(_ networks: [WifiNetwork]) -> Data {
        precondition(networks.count <= WifiNetwork.maxCount)
        var packet = Data([InstallerOp.wifiSet.rawValue, UInt8(networks.count)])
        for net in networks {
            let ssid = Data(net.ssid.utf8), pass = Data(net.password.utf8)
            packet.append(UInt8(ssid.count))
            packet.append(ssid)
            packet.append(UInt8(pass.count))
            packet.append(pass)
        }
        return packet
    }

    /// UTF-8 prefix of at most `maxBytes`, never splitting a character.
    public static func truncatedUTF8(_ text: String, maxBytes: Int) -> Data {
        var out = Data()
        for scalar in text.unicodeScalars {
            let bytes = Data(String(scalar).utf8)
            if out.count + bytes.count > maxBytes { break }
            out.append(bytes)
        }
        return out
    }
}

public enum InstallerPhase: UInt8 {
    case idle = 0, receiving, verifying, done, failed
}

/// inst_err_t
public enum InstallerError: UInt8 {
    case ok = 0, state, size, sequence, table, flash, verify, image

    public var message: String {
        switch self {
        case .ok: return "正常"
        case .state: return "设备当前状态不能执行这个操作"
        case .size: return "固件大小超出设备可用空间"
        case .sequence: return "数据顺序错乱"
        case .table: return "分区表不兼容"
        case .flash: return "设备 Flash 写入失败"
        case .verify: return "写入后校验不一致"
        case .image: return "固件中的应用镜像无效"
        }
    }
}

/// inst_table_err_t
public enum TableError: UInt8, Error {
    case ok = 0, format, entry, overlap, reserved, noApp, imageEnd, full

    public var message: String {
        switch self {
        case .ok: return "兼容"
        case .format: return "不是有效的 ESP-IDF 分区表"
        case .entry: return "分区表中有无效或加密的分区"
        case .overlap: return "分区重叠或名称重复"
        case .reserved: return "固件需要使用安装器所在的 Flash 区域"
        case .noApp: return "没有可启动的应用分区"
        case .imageEnd: return "固件数据超出可用空间（约 6.99 MB）"
        case .full: return "分区表条目过多"
        }
    }
}

public struct InstallerStatus: Equatable {
    public var phase: InstallerPhase
    public var error: InstallerError
    public var tableError: TableError
    public var resumed: Bool
    public var resync: Bool
    public var position: UInt32
    public var end: UInt32
    public var sequence: UInt32

    public init?(_ data: Data) {
        guard data.count >= 16 else { return nil }
        let b = [UInt8](data)
        phase = InstallerPhase(rawValue: b[0]) ?? .idle
        error = InstallerError(rawValue: b[1]) ?? .flash
        tableError = TableError(rawValue: b[2]) ?? .format
        resumed = b[3] & 0x01 != 0
        resync = b[3] & 0x02 != 0
        position = b.le32(4)
        end = b.le32(8)
        sequence = b.le32(12)
    }
}

public struct InstallerInfo: Equatable {
    public var protocolVersion: UInt8
    public var hasRecord: Bool
    public var interrupted: Bool
    public var appPresent: Bool
    public var streamStart: UInt32
    public var playLimit: UInt32
    public var installedEnd: UInt32
    public var installedSHA256: Data
    public var installedName: String
    public var firmwareVersion: String

    public init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 50 else { return nil }
        protocolVersion = b[0]
        hasRecord = b[1] & 0x01 != 0
        interrupted = b[1] & 0x02 != 0
        appPresent = b[1] & 0x04 != 0
        streamStart = b.le32(4)
        playLimit = b.le32(8)
        installedEnd = b.le32(12)
        installedSHA256 = Data(b[16..<48])
        let nameLen = Int(b[48])
        guard b.count >= 49 + nameLen + 1 else { return nil }
        installedName = String(decoding: b[49..<(49 + nameLen)], as: UTF8.self)
        let verAt = 49 + nameLen
        let verLen = Int(b[verAt])
        guard b.count >= verAt + 1 + verLen else { return nil }
        firmwareVersion = String(decoding: b[(verAt + 1)..<(verAt + 1 + verLen)], as: UTF8.self)
    }
}

/// A saved network; mirrors components/installer_core/include/inst_wifi.h.
public struct WifiNetwork: Codable, Equatable, Hashable, Identifiable {
    public static let maxCount = 5
    public var id: String { ssid }
    public var ssid: String
    public var password: String

    public init(ssid: String, password: String) {
        self.ssid = ssid
        self.password = password
    }

    /// nil when the device will accept it, otherwise a reason in Chinese.
    public var problem: String? {
        let ssidBytes = ssid.utf8.count, passBytes = password.utf8.count
        if ssidBytes == 0 { return "请输入网络名称" }
        if ssidBytes > 32 { return "网络名称超过 32 字节" }
        if passBytes == 0 { return nil }
        if password.utf8.contains(0) { return "密码包含无效字符" }
        if passBytes < 8 { return "密码至少 8 个字符（开放网络请留空）" }
        if passBytes > 64 { return "密码超过 64 个字符" }
        if passBytes == 64 && !password.allSatisfy(\.isHexDigit) { return "64 位密码必须是十六进制 PSK" }
        return nil
    }
}

public enum WifiResult: UInt8 {
    case none = 0, saved, invalid, failed

    public var message: String {
        switch self {
        case .none: return "未保存"
        case .saved: return "已保存"
        case .invalid: return "设备拒绝了网络列表"
        case .failed: return "设备 Flash 写入失败"
        }
    }
}

/// WIFI characteristic: saved SSIDs (no passwords) and the last save result.
public struct InstallerWifiList: Equatable {
    public var result: WifiResult
    public var applied: Bool
    public var operations: UInt32
    public var ssids: [String]

    public init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 8, b[0] == 1 else { return nil }
        result = WifiResult(rawValue: b[1]) ?? .failed
        applied = b[2] & 0x01 != 0
        operations = b.le32(4)
        var ssids: [String] = []
        var at = 8
        for _ in 0..<Int(b[3]) {
            guard at < b.count else { return nil }
            let len = Int(b[at])
            guard at + 1 + len <= b.count else { return nil }
            ssids.append(String(decoding: b[(at + 1)..<(at + 1 + len)], as: UTF8.self))
            at += 1 + len
        }
        self.ssids = ssids
    }
}

extension Data {
    mutating func appendLE(_ value: UInt32) {
        append(contentsOf: [UInt8(value & 0xFF), UInt8(value >> 8 & 0xFF),
                            UInt8(value >> 16 & 0xFF), UInt8(value >> 24 & 0xFF)])
    }
}

extension Array where Element == UInt8 {
    func le32(_ at: Int) -> UInt32 {
        UInt32(self[at]) | UInt32(self[at + 1]) << 8 | UInt32(self[at + 2]) << 16 | UInt32(self[at + 3]) << 24
    }
}
