// The user's saved firmware: an index.json plus one .bin per item, stored in
// the app's Application Support directory.
import CryptoKit
import Foundation

public struct FirmwareItem: Codable, Identifiable, Equatable {
    public enum Source: Codable, Equatable {
        case local(fileName: String)
        case community(slug: String, playID: Int, author: String?, shareVersion: String?)
    }

    public var id: UUID
    public var name: String
    public var source: Source
    public var kind: FirmwareKind
    public var fileSize: Int
    public var dataEnd: UInt32
    public var fileSHA256: String
    public var streamSHA256: String
    public var compatibilityError: String?
    public var notes: String
    public var addedAt: Date
    public var updatedAt: Date

    public var isCommunity: Bool {
        if case .community = source { return true }
        return false
    }

    public var communitySlug: String? {
        if case .community(let slug, _, _, _) = source { return slug }
        return nil
    }
}

public enum LibraryError: Error, Equatable {
    case missingFile
    case invalid(String)

    public var message: String {
        switch self {
        case .missingFile: return "固件文件丢失，请重新添加"
        case .invalid(let reason): return reason
        }
    }
}

public final class FirmwareLibrary {
    public let directory: URL
    public private(set) var items: [FirmwareItem] = []

    private var indexURL: URL { directory.appendingPathComponent("index.json") }

    public init(directory: URL) throws {
        self.directory = directory
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        if let data = try? Data(contentsOf: indexURL) {
            let decoder = JSONDecoder()
            items = (try? decoder.decode([FirmwareItem].self, from: data)) ?? []
        }
    }

    public static func defaultDirectory() throws -> URL {
        try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                    appropriateFor: nil, create: true)
            .appendingPathComponent("Firmware", isDirectory: true)
    }

    public func fileURL(for item: FirmwareItem) -> URL {
        directory.appendingPathComponent("\(item.id.uuidString).bin")
    }

    public func data(for item: FirmwareItem) throws -> Data {
        guard let data = try? Data(contentsOf: fileURL(for: item)) else { throw LibraryError.missingFile }
        return data
    }

    public func image(for item: FirmwareItem) throws -> FirmwareImage {
        let data = try data(for: item)
        do {
            return try FirmwareImage(fileData: data)
        } catch let error as FirmwareImageError {
            throw LibraryError.invalid(error.message)
        }
    }

    /// Adds firmware, or updates the existing entry with the same content or
    /// the same community play. Returns the stored item.
    @discardableResult
    public func add(data: Data, name: String, source: FirmwareItem.Source, now: Date = Date()) throws -> FirmwareItem {
        let image: FirmwareImage
        do {
            image = try FirmwareImage(fileData: data)
        } catch let error as FirmwareImageError {
            throw LibraryError.invalid(error.message)
        }
        let fileSHA = Data(SHA256.hash(data: data)).hexString
        var item: FirmwareItem
        if let index = items.firstIndex(where: { existing in
            existing.fileSHA256 == fileSHA ||
                (existing.communitySlug != nil && existing.communitySlug == FirmwareLibrary.slug(of: source))
        }) {
            item = items[index]
            item.source = source
            item.updatedAt = now
            if case .community = source { item.name = name }
        } else {
            item = FirmwareItem(id: UUID(), name: name, source: source, kind: image.kind, fileSize: data.count,
                                dataEnd: image.dataEnd, fileSHA256: fileSHA,
                                streamSHA256: image.streamSHA256.hexString, compatibilityError: nil,
                                notes: "", addedAt: now, updatedAt: now)
        }
        item.kind = image.kind
        item.fileSize = data.count
        item.dataEnd = image.dataEnd
        item.fileSHA256 = fileSHA
        item.streamSHA256 = image.streamSHA256.hexString
        item.compatibilityError = image.tableError?.message
        try data.write(to: fileURL(for: item), options: .atomic)
        if let index = items.firstIndex(where: { $0.id == item.id }) {
            items[index] = item
        } else {
            items.insert(item, at: 0)
        }
        try save()
        return item
    }

    public func update(_ item: FirmwareItem) throws {
        guard let index = items.firstIndex(where: { $0.id == item.id }) else { return }
        items[index] = item
        try save()
    }

    public func remove(_ item: FirmwareItem) throws {
        items.removeAll { $0.id == item.id }
        try? FileManager.default.removeItem(at: fileURL(for: item))
        try save()
    }

    /// The library item whose stream hash matches what the device reports.
    public func item(matchingStreamSHA256 sha: Data) -> FirmwareItem? {
        let hex = sha.hexString
        return items.first { $0.streamSHA256 == hex }
    }

    private func save() throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(items).write(to: indexURL, options: .atomic)
    }

    private static func slug(of source: FirmwareItem.Source) -> String? {
        if case .community(let slug, _, _, _) = source { return slug }
        return nil
    }
}
