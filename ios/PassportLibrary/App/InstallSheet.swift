import PassportKit
import SwiftUI

struct InstallSheet: View {
    let item: FirmwareItem
    let image: FirmwareImage
    @Environment(DeviceManager.self) private var device
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("要安装的固件") {
                    LabeledContent("名称", value: item.name)
                    LabeledContent("传输大小", value: Format.bytes(image.dataEnd - InstallerLayout.streamStart))
                }
                if let install = device.install, install.name == item.name {
                    progressSection(install)
                } else if device.isReady {
                    Section("目标设备") { DeviceInfoSection() }
                    Section {
                        Button {
                            device.beginInstall(image: image, name: item.name)
                        } label: {
                            Text("开始安装").font(.headline).frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        .listRowBackground(Color.clear)
                        .listRowInsets(EdgeInsets())
                    } footer: {
                        Text("会覆盖设备上当前的固件和它保存的数据（NVS）。安装过程中请保持设备开机、手机靠近设备。")
                    }
                } else {
                    Section("连接设备") { DeviceConnectSection() }
                }
            }
            .navigationTitle("安装")
            .navigationBarTitleDisplayMode(.inline)
            .interactiveDismissDisabled(device.install?.isActive == true)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    if device.install?.isActive == true {
                        Button("停止", role: .destructive) { device.cancelInstall() }
                    } else {
                        Button("关闭") {
                            device.dismissInstall()
                            dismiss()
                        }
                    }
                }
            }
            .onAppear {
                // Keep an interrupted install of this item so it can resume.
                if device.install?.isActive != true && device.install?.name != item.name {
                    device.dismissInstall()
                }
                device.startScan()
            }
        }
    }

    @ViewBuilder
    private func progressSection(_ install: DeviceManager.InstallState) -> some View {
        Section {
            switch install.phase {
            case .starting:
                HStack { ProgressView(); Text("正在准备…") }
            case .transferring:
                VStack(alignment: .leading, spacing: 8) {
                    ProgressView(value: install.fraction)
                    HStack {
                        Text("\(Int(install.fraction * 100))%").monospacedDigit()
                        Spacer()
                        Text("\(Format.bytes(install.sent)) / \(Format.bytes(install.total))")
                            .monospacedDigit()
                    }
                    .font(.callout)
                    if install.bytesPerSecond > 0 {
                        let remaining = Double(install.total - install.sent) / install.bytesPerSecond
                        Text("\(Format.bytes(Int(install.bytesPerSecond)))/s · 还需约 \(Format.duration(remaining))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    if install.resumed {
                        Text("已从上次断开的位置继续").font(.caption).foregroundStyle(.secondary)
                    }
                }
            case .verifying:
                HStack { ProgressView(); Text("设备正在校验写入的数据…") }
            case .done:
                Label("安装完成，设备正在启动新固件", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
            case .failed(let reason):
                Label(reason, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
                if device.canResume {
                    Button("继续安装") { device.resumeInstall() }
                } else if !device.isReady {
                    DeviceConnectSection()
                }
            }
        } header: {
            Text("安装进度")
        } footer: {
            if install.isActive {
                Text("可以锁屏，但请不要退出 App。断开后重新连接，可以从断点继续。")
            }
        }
    }
}
