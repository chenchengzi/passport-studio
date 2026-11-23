// Mirror of components/passport_link/include/link_core.h and the message
// format in docs/development/engineering/passport-link.md.
import Foundation

public enum LinkUUID {
    public static let service = "CDDE7C00-DD5A-47C6-890F-268DE0F6918A"
    public static let info = "CDDE7C01-DD5A-47C6-890F-268DE0F6918A"
    public static let rx = "CDDE7C02-DD5A-47C6-890F-268DE0F6918A"
    public static let tx = "CDDE7C03-DD5A-47C6-890F-268DE0F6918A"
}

public enum LinkFrame {
    public static let protocolVersion = 1
    public static let messageMax = 4096
    public static let first: UInt8 = 0x01
    public static let last: UInt8 = 0x02

    /// Split a message into frames of at most `maxFrame` bytes (flags included).
    public static func split(_ message: Data, maxFrame: Int) -> [Data] {
        precondition(maxFrame >= 2 && !message.isEmpty)
        var frames: [Data] = []
        var offset = 0
        let bytes = [UInt8](message)
        while offset < bytes.count {
            let n = min(maxFrame - 1, bytes.count - offset)
            var flags: UInt8 = offset == 0 ? first : 0
            if offset + n == bytes.count { flags |= last }
            frames.append(Data([flags] + bytes[offset..<(offset + n)]))
            offset += n
        }
        return frames
    }
}

/// Reassembles TX notifications into messages; same rules as link_rx_feed().
public struct LinkReassembler {
    private var buffer = Data()
    private var active = false
    private var overflow = false

    public init() {}

    /// Returns a complete message, or nil while partial or after a drop.
    public mutating func feed(_ frame: Data) -> Data? {
        guard let flags = frame.first, flags & ~(LinkFrame.first | LinkFrame.last) == 0 else {
            active = false
            return nil
        }
        if flags & LinkFrame.first != 0 {
            buffer.removeAll(keepingCapacity: true)
            active = true
            overflow = false
        } else if !active {
            return nil
        }
        let payload = frame.dropFirst()
        if buffer.count + payload.count > LinkFrame.messageMax { overflow = true }
        if !overflow { buffer.append(payload) }
        guard flags & LinkFrame.last != 0 else { return nil }
        active = false
        if overflow || buffer.isEmpty { return nil }
        return buffer
    }
}

/// A setting value as carried in JSON.
public enum LinkValue: Equatable, Hashable, Codable, CustomStringConvertible {
    case bool(Bool)
    case int(Int)
    case text(String)

    public init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let b = try? c.decode(Bool.self) { self = .bool(b) }
        else if let i = try? c.decode(Int.self) { self = .int(i) }
        else { self = .text(try c.decode(String.self)) }
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .bool(let b): try c.encode(b)
        case .int(let i): try c.encode(i)
        case .text(let s): try c.encode(s)
        }
    }

    var json: Any {
        switch self {
        case .bool(let b): return b
        case .int(let i): return i
        case .text(let s): return s
        }
    }

    static func from(json: Any) -> LinkValue? {
        if let n = json as? NSNumber {
            // JSONSerialization returns booleans as NSNumber too.
            if CFGetTypeID(n) == CFBooleanGetTypeID() { return .bool(n.boolValue) }
            return .int(n.intValue)
        }
        if let s = json as? String { return .text(s) }
        return nil
    }

    public var description: String {
        switch self {
        case .bool(let b): return b ? "开" : "关"
        case .int(let i): return String(i)
        case .text(let s): return s
        }
    }
}

public struct LinkSetting: Decodable, Equatable, Identifiable {
    public enum Kind: String, Decodable { case bool, int, `enum`, text }

    public var id: String { key }
    public let key: String
    public let label: String
    public let kind: Kind
    public let defaultValue: LinkValue
    public let min: Int?
    public let max: Int?
    public let options: [String]?

    enum CodingKeys: String, CodingKey {
        case key = "k", label = "n", kind = "type", defaultValue = "def", min, max, options = "opt"
    }

    /// Mirrors link_setting_int_ok / link_setting_str_ok.
    public func accepts(_ value: LinkValue) -> Bool {
        switch (kind, value) {
        case (.bool, .bool): return true
        case (.int, .int(let i)): return i >= (min ?? Int.min) && i <= (max ?? Int.max)
        case (.enum, .text(let s)): return options?.contains(s) ?? false
        case (.text, .text(let s)): return s.utf8.count <= (max ?? Int.max)
        default: return false
        }
    }
}

/// The INFO characteristic.
public struct LinkInfo: Decodable, Equatable {
    public let version: Int
    public let appID: String
    public let name: String
    public let appVersion: String
    public let channels: [String]
    public let settings: [LinkSetting]

    enum CodingKeys: String, CodingKey {
        case version = "v", appID = "id", name, appVersion = "ver", channels = "ch", settings = "set"
    }

    public init(_ data: Data) throws {
        self = try JSONDecoder().decode(LinkInfo.self, from: data)
    }
}

/// Messages from the device.
public enum LinkIncoming: Equatable {
    case settings([String: LinkValue])
    case error(why: String, key: String?)
    case request(channel: String)

    public init?(_ data: Data) {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let t = obj["t"] as? String else { return nil }
        switch t {
        case "settings":
            guard let v = obj["v"] as? [String: Any] else { return nil }
            self = .settings(v.compactMapValues(LinkValue.from(json:)))
        case "err":
            self = .error(why: obj["why"] as? String ?? "", key: obj["k"] as? String)
        case "req":
            guard let ch = obj["ch"] as? String else { return nil }
            self = .request(channel: ch)
        default:
            return nil
        }
    }
}

/// Messages to the device.
public enum LinkOutgoing {
    case get
    case set([String: LinkValue])
    case push(channel: String, data: [String: Any])

    public var data: Data {
        let obj: [String: Any]
        switch self {
        case .get: obj = ["t": "get"]
        case .set(let values): obj = ["t": "set", "v": values.mapValues(\.json)]
        case .push(let channel, let data): obj = ["t": "push", "ch": channel, "d": data]
        }
        return (try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])) ?? Data()
    }

    public static func time(_ date: Date = Date(), timeZone: TimeZone = .current) -> LinkOutgoing {
        .push(channel: "time", data: ["unix": Int(date.timeIntervalSince1970),
                                      "tz": timeZone.secondsFromGMT(for: date) / 60])
    }

    /// `name` is English (Latin fonts); `nameZh` is the Chinese place name, if any.
    public static func location(latitude: Double, longitude: Double, accuracy: Double, name: String,
                                nameZh: String? = nil) -> LinkOutgoing {
        // About 1 m of precision is plenty for weather and keeps messages short.
        func round5(_ x: Double) -> Double { (x * 100_000).rounded() / 100_000 }
        var data: [String: Any] = ["lat": round5(latitude), "lon": round5(longitude),
                                   "acc": Int(accuracy.rounded()), "name": name]
        if let nameZh, !nameZh.isEmpty { data["name_zh"] = nameZh }
        return .push(channel: "location", data: data)
    }
}
