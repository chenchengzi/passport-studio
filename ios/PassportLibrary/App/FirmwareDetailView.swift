import PassportKit
import SwiftUI

struct FirmwareDetailView: View {
    let itemID: UUID
    @Environment(LibraryModel.self) private var library
    @Environment(DeviceManager.self) private var device
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var notes = ""
    @State private var installing = false
    @State private var confirmDelete = false
    @State private var refreshing = false
    @State private var refreshProgress = 0.0
    @State private var refreshMessage: String?
    @State private var image: FirmwareImage?
    @State private var imageError: String?

    var body: some View {
        if let item = library.item(id: itemID) {
            content(item)
        } else {
            ContentUnavailableView("固件已删除", systemImage: "trash")
        }
    }

    private func content(_ item: FirmwareItem) -> some View {
        Form {
            Section {
                TextField("名称", text: $name)
                    .font(.title3.weight(.semibold))
                    .onSubmit { library.rename(item, to: name) }
                Label(item.sourceDescription, systemImage: item.sourceSymbol)
                    .foregroundStyle(.secondary)
            }

            Section {
                Button {
                    installing = true
                } label: {
                    Label("安装到设备", systemImage: "arrow.down.to.line.circle.fill")
                        .font(.headline)
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(item.compatibilityError != nil || image == nil)
                .listRowBackground(Color.clear)
                .listRowInsets(EdgeInsets())
                compatibility(item)
            }

            Section("固件信息") {
                LabeledContent("类型", value: item.kind == .merged ? "合并固件（0x0）" : "应用镜像（按默认分区表安装）")
                LabeledContent("文件大小", value: Format.bytes(item.fileSize))
                LabeledContent("需要传输", value: Format.bytes(item.dataEnd - InstallerLayout.streamStart))
                LabeledContent("SHA-256") {
                    Text(Format.shortHash(item.fileSHA256)).font(.caption.monospaced())
                }
                LabeledContent("添加时间", value: Format.date(item.addedAt))
                if let image, !image.partitions.isEmpty {
                    DisclosureGroup("分区表（\(image.partitions.count) 项）") {
                        ForEach(image.partitions, id: \.label) { p in
                            HStack {
                                Text(p.label).font(.callout.monospaced())
                                Spacer()
                                Text(String(format: "0x%X · %@", p.offset, Format.bytes(p.size)))
                                    .font(.caption.monospaced())
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }

            if case .community(let slug, _, let author, let version) = item.source {
                Section("社区玩法") {
                    if let author { LabeledContent("作者", value: author) }
                    if let version { LabeledContent("版本", value: version) }
                    ShareLink("分享玩法链接", item: CommunityPlay.link(slug: slug))
                    Button {
                        Task { await refresh(item) }
                    } label: {
                        if refreshing {
                            ProgressView(value: refreshProgress) { Text("正在检查更新…") }
                        } else {
                            Label("检查更新", systemImage: "arrow.clockwise")
                        }
                    }
                    .disabled(refreshing)
                    if let refreshMessage {
                        Text(refreshMessage).font(.footnote).foregroundStyle(.secondary)
                    }
                }
            }

            Section("备注") {
                TextField("比如：这一版改了什么", text: $notes, axis: .vertical)
                    .lineLimit(2...6)
                    .onChange(of: notes) { library.setNotes(item, notes) }
            }

            Section {
                Button("从固件库删除", role: .destructive) { confirmDelete = true }
            }
        }
        .navigationTitle(item.name)
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            name = item.name
            notes = item.notes
            loadImage(item)
        }
        .onChange(of: item.fileSHA256) { loadImage(item) }
        .onDisappear { if name != item.name { library.rename(item, to: name) } }
        .sheet(isPresented: $installing) {
            if let image { InstallSheet(item: item, image: image) }
        }
        .confirmationDialog("删除「\(item.name)」？", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("删除", role: .destructive) {
                library.delete(item)
                dismiss()
            }
        } message: {
            Text("只会从手机上的固件库删除，不影响设备上已经安装的固件。")
        }
    }

    @ViewBuilder
    private func compatibility(_ item: FirmwareItem) -> some View {
        if let imageError {
            Label(imageError, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.red)
        } else if let reason = item.compatibilityError {
            Label("不能安装：\(reason)", systemImage: "xmark.octagon.fill").foregroundStyle(.red)
        } else if let image, let boot = image.bootPartition {
            let room = InstallerLayout.playLimit - boot.offset
            Label("可以安装 · 应用分区可用 \(Format.bytes(min(boot.size, room)))", systemImage: "checkmark.seal.fill")
                .foregroundStyle(.green)
        }
    }

    private func loadImage(_ item: FirmwareItem) {
        do {
            image = try library.image(for: item)
            imageError = nil
        } catch let error as LibraryError {
            imageError = error.message
        } catch {
            imageError = error.localizedDescription
        }
    }

    private func refresh(_ item: FirmwareItem) async {
        refreshing = true
        refreshMessage = nil
        defer { refreshing = false }
        do {
            let updated = try await library.refresh(item) { value in
                Task { @MainActor in refreshProgress = value }
            }
            refreshMessage = updated ? "已下载新版本。" : "已经是最新版本。"
            if let current = library.item(id: item.id) { loadImage(current) }
        } catch let error as CommunityError {
            refreshMessage = error.message
        } catch {
            refreshMessage = error.localizedDescription
        }
    }
}

extension CommunityPlay {
    static func link(slug: String) -> URL {
        var components = URLComponents()
        components.scheme = "https"
        components.host = CommunityAPI.host
        components.path = "/plays/"
        components.queryItems = [URLQueryItem(name: "play", value: slug)]
        return components.url!
    }
}
