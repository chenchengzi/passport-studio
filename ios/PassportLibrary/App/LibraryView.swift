import PassportKit
import SwiftUI
import UniformTypeIdentifiers

struct LibraryView: View {
    @Environment(LibraryModel.self) private var library
    @Environment(DeviceManager.self) private var device
    @State private var importing = false
    @State private var addingLink = false
    @State private var showingDevice = false
    @State private var showingWifi = false
    @State private var showingLink = false
    @State private var pastedLink: String?

    var body: some View {
        NavigationStack {
            List {
                if library.items.isEmpty {
                    ContentUnavailableView {
                        Label("固件库是空的", systemImage: "shippingbox")
                    } description: {
                        Text("点右上角 + 导入自己编译的 .bin，或粘贴社区玩法链接。")
                    }
                    .listRowBackground(Color.clear)
                }
                if let current = library.item(matching: device.info) {
                    Section("设备上正在运行") {
                        NavigationLink(value: current.id) { FirmwareRow(item: current, onDevice: true) }
                    }
                }
                if !library.localItems.isEmpty {
                    Section("我的固件") { rows(library.localItems) }
                }
                if !library.communityItems.isEmpty {
                    Section("社区玩法") { rows(library.communityItems) }
                }
            }
            .navigationTitle("Passport 固件库")
            .navigationDestination(for: UUID.self) { id in
                FirmwareDetailView(itemID: id)
            }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button { showingDevice = true } label: { DeviceStatusLabel() }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showingLink = true } label: { Image(systemName: "dot.radiowaves.left.and.right") }
                        .accessibilityLabel("设备连接")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showingWifi = true } label: { Image(systemName: "wifi") }
                        .accessibilityLabel("Wi-Fi 配网")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button("导入本地固件文件", systemImage: "folder") { importing = true }
                        Button("添加社区玩法链接", systemImage: "link") {
                            pastedLink = nil
                            addingLink = true
                        }
                        if UIPasteboard.general.hasStrings || UIPasteboard.general.hasURLs {
                            Button("粘贴剪贴板中的链接", systemImage: "doc.on.clipboard") {
                                pastedLink = UIPasteboard.general.url?.absoluteString ?? UIPasteboard.general.string
                                addingLink = true
                            }
                        }
                    } label: {
                        Image(systemName: "plus")
                    }
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.data],
                          allowsMultipleSelection: true) { result in
                if case .success(let urls) = result {
                    urls.forEach { library.importFile(at: $0) }
                }
            }
            .sheet(isPresented: $addingLink) {
                AddCommunitySheet(initialText: pastedLink ?? "")
            }
            .sheet(isPresented: $showingDevice) {
                DeviceSheet()
            }
            .sheet(isPresented: $showingWifi) {
                WifiSheet()
            }
            .sheet(isPresented: $showingLink) {
                LinkSheet()
            }
            .alert("出错了", isPresented: Binding(get: { library.lastError != nil },
                                                set: { if !$0 { library.lastError = nil } })) {
                Button("好") { library.lastError = nil }
            } message: {
                Text(library.lastError ?? "")
            }
        }
    }

    @ViewBuilder
    private func rows(_ items: [FirmwareItem]) -> some View {
        ForEach(items) { item in
            NavigationLink(value: item.id) {
                FirmwareRow(item: item, onDevice: library.item(matching: device.info)?.id == item.id)
            }
        }
        .onDelete { offsets in
            offsets.map { items[$0] }.forEach(library.delete)
        }
    }
}

struct FirmwareRow: View {
    let item: FirmwareItem
    let onDevice: Bool

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: item.sourceSymbol)
                .font(.title3)
                .foregroundStyle(item.isCommunity ? Color.teal : Color.indigo)
                .frame(width: 32)
            VStack(alignment: .leading, spacing: 3) {
                Text(item.name).font(.body.weight(.medium)).lineLimit(1)
                Text("\(Format.bytes(item.dataEnd)) · \(Format.date(item.updatedAt))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if onDevice {
                StatusBadge(text: "设备上", color: .green)
            } else if item.compatibilityError != nil {
                StatusBadge(text: "不兼容", color: .red)
            }
        }
    }
}

struct DeviceStatusLabel: View {
    @Environment(DeviceManager.self) private var device

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(color).frame(width: 8, height: 8)
            Text(text).font(.subheadline)
        }
    }

    private var color: Color {
        switch device.connection {
        case .ready: return .green
        case .connecting, .pairing, .scanning: return .orange
        case .failed, .bluetoothOff, .unauthorized: return .red
        case .idle: return .gray
        }
    }

    private var text: String {
        if let install = device.install, install.isActive { return "安装中 \(Int(install.fraction * 100))%" }
        return device.deviceName ?? "设备"
    }
}
