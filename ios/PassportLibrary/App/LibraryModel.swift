import Foundation
import Observation
import PassportKit

/// UI-facing wrapper around FirmwareLibrary. Main actor only.
@MainActor
@Observable
final class LibraryModel {
    private(set) var items: [FirmwareItem] = []
    var lastError: String?
    private let library: FirmwareLibrary?
    private let api = CommunityAPI()

    init() {
        do {
            library = try FirmwareLibrary(directory: FirmwareLibrary.defaultDirectory())
        } catch {
            library = nil
            lastError = "无法打开固件库：\(error.localizedDescription)"
        }
        reload()
    }

    var localItems: [FirmwareItem] { items.filter { !$0.isCommunity } }
    var communityItems: [FirmwareItem] { items.filter(\.isCommunity) }

    private func reload() {
        items = library?.items ?? []
    }

    func item(id: UUID) -> FirmwareItem? {
        items.first { $0.id == id }
    }

    func image(for item: FirmwareItem) throws -> FirmwareImage {
        guard let library else { throw LibraryError.missingFile }
        return try library.image(for: item)
    }

    func item(matching info: InstallerInfo?) -> FirmwareItem? {
        guard let info, info.hasRecord else { return nil }
        return library?.item(matchingStreamSHA256: info.installedSHA256)
    }

    // MARK: Local files

    @discardableResult
    func importFile(at url: URL) -> FirmwareItem? {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            let name = url.deletingPathExtension().lastPathComponent
            let item = try library?.add(data: data, name: name, source: .local(fileName: url.lastPathComponent))
            reload()
            return item
        } catch let error as LibraryError {
            lastError = "\(url.lastPathComponent)：\(error.message)"
        } catch {
            lastError = "\(url.lastPathComponent)：\(error.localizedDescription)"
        }
        return nil
    }

    // MARK: Community plays

    func lookUp(_ reference: PlayReference) async throws -> CommunityPlay {
        try await api.play(for: reference)
    }

    func download(_ play: CommunityPlay, progress: @escaping @Sendable (Double) -> Void) async throws -> FirmwareItem {
        let data = try await api.download(play, progress: progress)
        guard let library else { throw LibraryError.missingFile }
        let item = try library.add(data: data, name: play.title.preferred.isEmpty ? play.slug : play.title.preferred,
                                   source: .community(slug: play.slug, playID: play.id, author: play.author,
                                                      shareVersion: play.shareVersion))
        reload()
        return item
    }

    /// Re-fetches a community play; returns true when a newer firmware was saved.
    func refresh(_ item: FirmwareItem, progress: @escaping @Sendable (Double) -> Void) async throws -> Bool {
        guard case .community(let slug, _, _, _) = item.source else { return false }
        let play = try await api.play(for: .slug(slug))
        if let sha = play.firmware?.sha256?.lowercased(), sha == item.fileSHA256 { return false }
        _ = try await download(play, progress: progress)
        return true
    }

    // MARK: Editing

    func rename(_ item: FirmwareItem, to name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, var current = self.item(id: item.id) else { return }
        current.name = trimmed
        save(current)
    }

    func setNotes(_ item: FirmwareItem, _ notes: String) {
        guard var current = self.item(id: item.id) else { return }
        current.notes = notes
        save(current)
    }

    func delete(_ item: FirmwareItem) {
        do {
            try library?.remove(item)
        } catch {
            lastError = "删除失败：\(error.localizedDescription)"
        }
        reload()
    }

    private func save(_ item: FirmwareItem) {
        do {
            try library?.update(item)
        } catch {
            lastError = "保存失败：\(error.localizedDescription)"
        }
        reload()
    }
}
