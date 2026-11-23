import PassportKit
import SwiftUI

/// Edit the prioritized network list and write it to the installer.
struct WifiSheet: View {
    @Environment(DeviceManager.self) private var device
    @Environment(WifiStore.self) private var wifi
    @Environment(\.dismiss) private var dismiss
    @State private var editing: EditTarget?

    struct EditTarget: Identifiable {
        let id = UUID()
        var original: WifiNetwork?
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    ForEach(Array(wifi.networks.enumerated()), id: \.element.id) { index, net in
                        Button {
                            editing = EditTarget(original: net)
                        } label: {
                            HStack {
                                Text("\(index + 1)").monospacedDigit().foregroundStyle(.secondary)
                                Text(net.ssid).foregroundStyle(.primary)
                                Spacer()
                                Image(systemName: net.password.isEmpty ? "lock.open" : "lock")
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                    .onDelete { wifi.remove(at: $0) }
                    .onMove { wifi.move(from: $0, to: $1) }
                    if !wifi.isFull {
                        Button("添加网络", systemImage: "plus") { editing = EditTarget(original: nil) }
                    }
                } header: {
                    Text("网络（按优先级）")
                } footer: {
                    Text("最多 \(WifiNetwork.maxCount) 个，拖动调整顺序。只支持 2.4 GHz 网络。密码保存在本机钥匙串中。")
                }

                Section {
                    deviceSection
                } header: {
                    Text("设备")
                } footer: {
                    Text("同步后，列表保存在设备上，所有固件都能通过 passport_wifi 组件读取。第一个网络还会写入当前固件的 Wi-Fi 设置，以后每次安装新固件也会自动写入。设备上的密码以明文保存在 Flash 中。")
                }
            }
            .navigationTitle("Wi-Fi 配网")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) { EditButton() }
                ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } }
            }
            .sheet(item: $editing) { target in
                WifiEditSheet(original: target.original)
            }
            .onAppear { device.startScan() }
            .onDisappear {
                device.dismissWifiResult()
                if !device.isReady { device.stopScan() }
            }
            .alert("出错了", isPresented: Binding(get: { wifi.lastError != nil },
                                                set: { if !$0 { wifi.lastError = nil } })) {
                Button("好") { wifi.lastError = nil }
            } message: {
                Text(wifi.lastError ?? "")
            }
        }
    }

    @ViewBuilder private var deviceSection: some View {
        if !device.isReady {
            DeviceConnectSection()
        } else if !device.supportsWifi {
            Label("设备上的安装器不支持配网，请用 USB 重新刷入首次安装镜像", systemImage: "exclamationmark.triangle")
                .foregroundStyle(.orange)
        } else {
            let onDevice = device.deviceWifi?.ssids ?? []
            LabeledContent("设备上的网络", value: onDevice.isEmpty ? "无" : onDevice.joined(separator: "、"))
            if onDevice != wifi.networks.map(\.ssid) {
                Label("与上面的列表不一致", systemImage: "arrow.triangle.2.circlepath")
                    .foregroundStyle(.orange)
            }
            Button {
                device.saveWifi(wifi.networks)
            } label: {
                if device.wifiSync == .saving {
                    HStack { ProgressView(); Text("正在保存…") }
                } else {
                    Label(wifi.networks.isEmpty ? "清除设备上的网络" : "同步到设备", systemImage: "square.and.arrow.down")
                }
            }
            .disabled(device.wifiSync == .saving || device.install?.isActive == true)
            switch device.wifiSync {
            case .saved(let applied):
                Label(applied ? "已保存，并写入当前固件的 Wi-Fi 设置" : "已保存到设备", systemImage: "checkmark.circle")
                    .foregroundStyle(.green)
            case .failed(let reason):
                Label(reason, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
            case .idle, .saving:
                EmptyView()
            }
        }
    }
}

struct WifiEditSheet: View {
    @Environment(WifiStore.self) private var wifi
    @Environment(\.dismiss) private var dismiss
    let original: WifiNetwork?
    @State private var ssid = ""
    @State private var password = ""
    @State private var showPassword = false

    private var draft: WifiNetwork { WifiNetwork(ssid: ssid, password: password) }
    private var duplicate: Bool {
        ssid != original?.ssid && wifi.networks.contains { $0.ssid == ssid }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("网络名称（SSID）", text: $ssid)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    HStack {
                        Group {
                            if showPassword {
                                TextField("密码（开放网络留空）", text: $password)
                            } else {
                                SecureField("密码（开放网络留空）", text: $password)
                            }
                        }
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        Button { showPassword.toggle() } label: {
                            Image(systemName: showPassword ? "eye.slash" : "eye")
                        }
                        .buttonStyle(.borderless)
                    }
                } footer: {
                    if !ssid.isEmpty, let problem = draft.problem {
                        Text(problem).foregroundStyle(.red)
                    } else if duplicate {
                        Text("列表中已有同名网络，保存会覆盖它").foregroundStyle(.orange)
                    }
                }
            }
            .navigationTitle(original == nil ? "添加网络" : "编辑网络")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("取消") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("保存") {
                        wifi.upsert(draft, replacing: original?.ssid)
                        dismiss()
                    }
                    .disabled(draft.problem != nil)
                }
            }
            .onAppear {
                ssid = original?.ssid ?? ""
                password = original?.password ?? ""
            }
        }
    }
}
