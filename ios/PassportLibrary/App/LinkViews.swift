import PassportKit
import SwiftUI

/// Device Link: running applications that use Passport Link.
struct LinkSheet: View {
    @Environment(LinkManager.self) private var link
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                if !link.bluetoothReady {
                    Label("请在控制中心打开蓝牙，并允许本 App 使用蓝牙", systemImage: "antenna.radiowaves.left.and.right.slash")
                }
                if !link.orderedSessions.isEmpty {
                    Section("我的设备") {
                        ForEach(link.orderedSessions) { session in
                            NavigationLink(value: session.id) { LinkRow(session: session) }
                        }
                    }
                }
                Section {
                    let fresh = link.nearby.filter { link.sessions[$0.id] == nil }
                    if fresh.isEmpty {
                        HStack { ProgressView(); Text("正在寻找设备…") }
                    }
                    ForEach(fresh) { device in
                        Button {
                            link.connect(device)
                        } label: {
                            HStack {
                                Image(systemName: "dot.radiowaves.left.and.right")
                                Text(device.name)
                                Spacer()
                                Text("连接").foregroundStyle(.secondary)
                            }
                        }
                    }
                } header: {
                    Text("附近")
                } footer: {
                    Text("这里列出正在运行、支持 Passport Link 的应用（例如天气示例），不是安装模式。第一次连接要输入设备屏幕上的 6 位配对码，之后会自动重连。")
                }
            }
            .navigationTitle("设备连接")
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(for: UUID.self) { id in
                if let session = link.sessions[id] { LinkDetailView(session: session) }
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } }
            }
            .onAppear { link.startScan() }
            .onDisappear { link.stopScan() }
        }
    }
}

private struct LinkRow: View {
    let session: LinkSession

    var body: some View {
        HStack {
            Image(systemName: "cpu")
            VStack(alignment: .leading) {
                Text(session.name)
                Text(status).font(.caption).foregroundStyle(color)
            }
        }
    }

    private var status: String {
        switch session.state {
        case .offline: return "不在范围内，靠近后自动连接"
        case .connecting: return "正在连接…"
        case .pairing: return "正在配对，输入设备屏幕上的配对码"
        case .ready: return "已连接"
        case .failed(let why): return why
        }
    }

    private var color: Color {
        switch session.state {
        case .ready: return .green
        case .failed: return .red
        default: return .secondary
        }
    }
}

struct LinkDetailView: View {
    @Environment(LinkManager.self) private var link
    @Environment(\.dismiss) private var dismiss
    let session: LinkSession

    var body: some View {
        Form {
            Section {
                LinkRow(session: session)
                if let info = session.info {
                    LabeledContent("应用", value: info.appID)
                    LabeledContent("版本", value: info.appVersion)
                }
                if case .failed = session.state {
                    Button("重新连接", systemImage: "arrow.clockwise") { link.reconnect(session) }
                }
            }

            if let info = session.info, !info.settings.isEmpty {
                Section {
                    ForEach(info.settings) { setting in
                        LinkSettingRow(session: session, setting: setting)
                    }
                    if let error = session.lastError {
                        Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
                    }
                } header: {
                    Text("设置")
                } footer: {
                    Text("修改后立即发送到设备，设备保存后生效。")
                }
                .disabled(session.state != .ready)
            }

            if session.uses("location") || session.uses("time") {
                Section("手机提供的数据") {
                    if session.uses("location") { locationRow }
                    if session.uses("time") {
                        LabeledContent("时间") {
                            Text(session.lastTimeSync.map { "已同步 \($0.formatted(date: .omitted, time: .shortened))" } ?? "连接后自动同步")
                        }
                    }
                }
            }

            Section {
                Button("忘记此设备", systemImage: "trash", role: .destructive) {
                    link.forget(session)
                    dismiss()
                }
            } footer: {
                Text("忘记后不再自动连接。如需重新配对，也请在 iPhone「设置 › 蓝牙」中忽略该设备。")
            }
        }
        .navigationTitle(session.name)
        .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder private var locationRow: some View {
        if link.location.denied {
            Label("定位权限被关闭，请在「设置 › 隐私与安全性 › 定位服务」中允许", systemImage: "location.slash")
                .foregroundStyle(.orange)
        } else {
            LabeledContent("位置") {
                Text(session.lastLocationName ?? link.location.current.map { "\($0.nameZh)（\($0.name)）" } ?? "正在定位…")
            }
            Button("立即推送位置", systemImage: "location") { link.pushLocation(to: session) }
                .disabled(session.state != .ready || link.location.current == nil)
        }
    }
}

/// One generated control per schema entry.
private struct LinkSettingRow: View {
    @Environment(LinkManager.self) private var link
    let session: LinkSession
    let setting: LinkSetting
    @State private var text = ""

    var body: some View {
        let value = session.value(for: setting)
        switch setting.kind {
        case .bool:
            Toggle(setting.label, isOn: Binding(
                get: { if case .bool(let b) = value { return b } else { return false } },
                set: { link.set(session, key: setting.key, value: .bool($0)) }))
        case .int:
            let current: Int = { if case .int(let i) = value { return i } else { return setting.min ?? 0 } }()
            Stepper(value: Binding(get: { current }, set: { link.set(session, key: setting.key, value: .int($0)) }),
                    in: (setting.min ?? 0)...(setting.max ?? 1_000_000),
                    step: stepSize) {
                LabeledContent(setting.label, value: String(current))
            }
        case .enum:
            Picker(setting.label, selection: Binding(
                get: { if case .text(let s) = value { return s } else { return "" } },
                set: { link.set(session, key: setting.key, value: .text($0)) })) {
                ForEach(setting.options ?? [], id: \.self) { Text($0).tag($0) }
            }
        case .text:
            TextField(setting.label, text: $text)
                .onSubmit { link.set(session, key: setting.key, value: .text(text)) }
                .onAppear { if case .text(let s) = value { text = s } }
                .submitLabel(.done)
        }
    }

    /// Bigger steps for wide ranges.
    private var stepSize: Int {
        let span = (setting.max ?? 100) - (setting.min ?? 0)
        return span > 100 ? 5 : 1
    }
}
