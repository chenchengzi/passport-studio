import PassportKit
import SwiftUI

/// Scan, connect and pair. Shared by the device sheet and the install sheet.
struct DeviceConnectSection: View {
    @Environment(DeviceManager.self) private var device

    var body: some View {
        switch device.connection {
        case .bluetoothOff:
            Label("请在控制中心打开蓝牙", systemImage: "antenna.radiowaves.left.and.right.slash")
        case .unauthorized:
            Label("请在「设置」中允许本 App 使用蓝牙", systemImage: "hand.raised")
        case .connecting(let name):
            HStack { ProgressView(); Text("正在连接 \(name)…") }
        case .pairing(let name):
            VStack(alignment: .leading, spacing: 6) {
                HStack { ProgressView(); Text("正在与 \(name) 配对") }
                Text("在弹出的蓝牙配对框里，输入设备屏幕上显示的 6 位配对码。每次连接配对码都会变。")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        case .ready:
            EmptyView()
        case .idle, .scanning, .failed:
            if case .failed(let reason) = device.connection {
                Label(reason, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
            }
            if device.discovered.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    HStack { ProgressView(); Text("正在寻找设备…") }
                    Text("设备关机后，按住上键再开机，直到屏幕出现 INSTALLER。")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            ForEach(device.discovered) { found in
                Button {
                    device.connect(found)
                } label: {
                    HStack {
                        Image(systemName: "cpu")
                        Text(found.name)
                        Spacer()
                        Image(systemName: found.rssi > -65 ? "wifi" : "wifi.exclamationmark")
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}

struct DeviceInfoSection: View {
    @Environment(DeviceManager.self) private var device
    @Environment(LibraryModel.self) private var library

    var body: some View {
        if let info = device.info {
            LabeledContent("设备", value: device.deviceName ?? "Passport")
            LabeledContent("安装器版本", value: info.firmwareVersion)
            if info.interrupted {
                Label("上次安装没有完成，请重新安装", systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
            } else if let item = library.item(matching: info) {
                LabeledContent("当前固件", value: item.name)
            } else if info.hasRecord {
                LabeledContent("当前固件", value: info.installedName.isEmpty ? "未在固件库中" : info.installedName)
            } else {
                LabeledContent("当前固件", value: info.appPresent ? "通过 USB 刷入" : "无")
            }
            LabeledContent("可用空间", value: Format.bytes(info.playLimit - info.streamStart))
        }
    }
}

struct DeviceSheet: View {
    @Environment(DeviceManager.self) private var device
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("连接") {
                    DeviceConnectSection()
                }
                if device.isReady {
                    Section("设备信息") { DeviceInfoSection() }
                    Section {
                        Button("启动设备上的固件", systemImage: "play.circle") { device.startInstalledApp() }
                            .disabled(device.info?.appPresent != true || device.info?.interrupted == true)
                        Button("断开连接", systemImage: "xmark.circle", role: .destructive) { device.disconnect() }
                    } footer: {
                        Text("启动后设备会离开安装模式；下次换固件时，再按住上键开机。")
                    }
                }
            }
            .navigationTitle("设备")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } }
            }
            .onAppear { device.startScan() }
            .onDisappear { if !device.isReady { device.stopScan() } }
        }
    }
}
