// FoloToy community play library (https://ai-passport.folotoy.cn).
// Links look like https://ai-passport.folotoy.cn/plays/?play=<slug>; the API
// answers /api/plays/<slug> and /api/plays/id/<number> with firmware metadata.
import CryptoKit
import Foundation

public enum PlayReference: Equatable {
    case slug(String)
    case id(Int)

    /// Accepts a share link, a URL with ?play=, a /plays/<slug> path, a bare
    /// slug, or a numeric project id. Text around a pasted link is ignored.
    public static func parse(_ input: String) -> PlayReference? {
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if let match = text.range(of: #"https?://[^\s]+"#, options: .regularExpression),
           let url = URL(string: String(text[match])) {
            guard let host = url.host?.lowercased(), host == CommunityAPI.host || host.hasSuffix(".folotoy.cn") else {
                return nil
            }
            let query = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
            for key in ["play", "slug", "id", "project"] {
                if let value = query.first(where: { $0.name == key })?.value, let ref = fromToken(value) {
                    return ref
                }
            }
            let parts = url.pathComponents.filter { $0 != "/" }
            if let index = parts.firstIndex(where: { $0 == "plays" || $0 == "play" }), index + 1 < parts.count {
                if parts[index + 1] == "id", index + 2 < parts.count { return fromToken(parts[index + 2]) }
                return fromToken(parts[index + 1])
            }
            return nil
        }
        return fromToken(text)
    }

    static func fromToken(_ token: String) -> PlayReference? {
        let t = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if let n = Int(t), n > 0 { return .id(n) }
        guard !t.isEmpty, t.count <= 80,
              t.range(of: #"^[A-Za-z0-9][A-Za-z0-9_-]*$"#, options: .regularExpression) != nil else { return nil }
        return .slug(t)
    }
}

public struct LocalizedText: Codable, Equatable {
    public var zh: String?
    public var en: String?
    public var preferred: String { [zh, en].compactMap { $0 }.first { !$0.isEmpty } ?? "" }
}

public struct CommunityPlay: Codable, Equatable {
    public struct Firmware: Codable, Equatable {
        public var available: Bool
        public var size: Int?
        public var sha256: String?
        public var url: String?
        public var format: String?
    }

    public var id: Int
    public var slug: String
    public var title: LocalizedText
    public var author: String?
    public var shareVersion: String?
    public var thumbnail: String?
    public var firmware: Firmware?

    public var shareLink: URL {
        var components = URLComponents()
        components.scheme = "https"
        components.host = CommunityAPI.host
        components.path = "/plays/"
        components.queryItems = [URLQueryItem(name: "play", value: slug)]
        return components.url!
    }
}

public enum CommunityError: Error, Equatable {
    case notFound
    case noFirmware
    case unsupportedFormat(String)
    case badResponse(Int)
    case integrity
    case unsafeURL

    public var message: String {
        switch self {
        case .notFound: return "玩法库里找不到这个玩法"
        case .noFirmware: return "这个玩法没有可下载的固件"
        case .unsupportedFormat(let f): return "不支持的固件格式：\(f)"
        case .badResponse(let code): return "服务器返回错误（\(code)）"
        case .integrity: return "下载的固件大小或 SHA-256 与玩法库不一致"
        case .unsafeURL: return "固件下载地址不在玩法库域名下"
        }
    }
}

public struct CommunityAPI {
    public static let host = "ai-passport.folotoy.cn"
    public var session: URLSession

    public init(session: URLSession = .shared) {
        self.session = session
    }

    public static func metadataURL(for reference: PlayReference) -> URL {
        var components = URLComponents()
        components.scheme = "https"
        components.host = host
        switch reference {
        case .slug(let slug): components.path = "/api/plays/\(slug)"
        case .id(let id): components.path = "/api/plays/id/\(id)"
        }
        return components.url!
    }

    /// Download URLs must stay on the play library host.
    public static func firmwareURL(for play: CommunityPlay) throws -> URL {
        guard let firmware = play.firmware, firmware.available, let path = firmware.url else {
            throw CommunityError.noFirmware
        }
        if let format = firmware.format, format != "esp-merged-0x0" {
            throw CommunityError.unsupportedFormat(format)
        }
        guard let url = URL(string: path, relativeTo: URL(string: "https://\(host)")!)?.absoluteURL,
              url.scheme == "https", url.host == host else {
            throw CommunityError.unsafeURL
        }
        return url
    }

    struct Envelope: Decodable {
        var ok: Bool?
        var play: CommunityPlay?
    }

    public static func decodePlay(_ data: Data) throws -> CommunityPlay {
        guard let play = try JSONDecoder().decode(Envelope.self, from: data).play else {
            throw CommunityError.notFound
        }
        return play
    }

    /// Checks a downloaded file against the library's published size and hash.
    public static func verify(_ data: Data, for play: CommunityPlay) throws {
        guard let firmware = play.firmware else { throw CommunityError.noFirmware }
        if let size = firmware.size, size != data.count { throw CommunityError.integrity }
        if let expected = firmware.sha256?.lowercased(),
           Data(SHA256.hash(data: data)).hexString != expected {
            throw CommunityError.integrity
        }
    }

    public func play(for reference: PlayReference) async throws -> CommunityPlay {
        let (data, response) = try await session.data(from: CommunityAPI.metadataURL(for: reference))
        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
        if code == 404 { throw CommunityError.notFound }
        guard code == 200 else { throw CommunityError.badResponse(code) }
        return try CommunityAPI.decodePlay(data)
    }

    public func download(_ play: CommunityPlay, progress: @escaping @Sendable (Double) -> Void) async throws -> Data {
        let url = try CommunityAPI.firmwareURL(for: play)
        let box = ObservationBox()
        let data: Data = try await withCheckedThrowingContinuation { continuation in
            let task = session.downloadTask(with: url) { file, response, error in
                box.observation = nil
                if let error {
                    continuation.resume(throwing: error)
                    return
                }
                let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                guard code == 200, let file else {
                    continuation.resume(throwing: CommunityError.badResponse(code))
                    return
                }
                // The temporary file is removed when this handler returns.
                do {
                    continuation.resume(returning: try Data(contentsOf: file))
                } catch {
                    continuation.resume(throwing: error)
                }
            }
            box.observation = task.progress.observe(\.fractionCompleted) { p, _ in
                progress(p.fractionCompleted)
            }
            task.resume()
        }
        progress(1)
        try CommunityAPI.verify(data, for: play)
        return data
    }
}

private final class ObservationBox: @unchecked Sendable {
    var observation: NSKeyValueObservation?
}
