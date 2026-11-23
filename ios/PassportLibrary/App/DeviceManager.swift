import CoreBluetooth
import Foundation
import Observation
import PassportKit
import UIKit

/// CoreBluetooth side of the installer protocol. All delegate callbacks are
/// delivered on the main queue, so state is only touched on the main actor.
@MainActor
@Observable
final class DeviceManager: NSObject {
    enum Connection: Equatable {
        case bluetoothOff
        case unauthorized
        case idle
        case scanning
        case connecting(String)
        case pairing(String)      // waiting for the encrypted INFO read
        case ready(String)
        case failed(String)
    }

    struct Discovered: Identifiable, Equatable {
        let id: UUID
        var name: String
        var rssi: Int
    }

    enum InstallPhase: Equatable {
        case starting
        case transferring
        case verifying
        case done
        case failed(String)
    }

    struct InstallState: Equatable {
        var name: String
        var phase: InstallPhase
        var sent: UInt32 = 0          // acknowledged stream bytes
        var total: UInt32
        var bytesPerSecond: Double = 0
        var resumed = false

        var fraction: Double { total == 0 ? 0 : Double(sent) / Double(total) }
        var isActive: Bool { phase == .starting || phase == .transferring || phase == .verifying }
    }

    private(set) var connection: Connection = .idle
    private(set) var discovered: [Discovered] = []
    private(set) var info: InstallerInfo?
    private(set) var install: InstallState?

    enum WifiSync: Equatable {
        case idle
        case saving
        case saved(applied: Bool)
        case failed(String)
    }

    /// Saved networks as reported by the device (SSIDs only).
    private(set) var deviceWifi: InstallerWifiList?
    private(set) var wifiSync: WifiSync = .idle

    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var peripheral: CBPeripheral?
    private var infoChar: CBCharacteristic?
    private var controlChar: CBCharacteristic?
    private var dataChar: CBCharacteristic?
    private var statusChar: CBCharacteristic?
    private var wifiChar: CBCharacteristic?
    private var wifiOpsBefore: UInt32 = 0
    private var wifiPoll: Timer?
    private var wantsScan = false

    private var planner: TransferPlanner?
    private var pendingImage: FirmwareImage?
    private var lastSequence: UInt32 = 0
    private var commitSent = false
    private var lastProgressAt = Date()
    private var rateMark: (time: Date, position: UInt32)?
    private var watchdog: Timer?

    private let serviceUUID = CBUUID(string: InstallerUUID.service)

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    var deviceName: String? {
        switch connection {
        case .connecting(let n), .pairing(let n), .ready(let n): return n
        default: return nil
        }
    }

    var isReady: Bool {
        if case .ready = connection { return true }
        return false
    }

    // MARK: - Scanning and connection

    func startScan() {
        wantsScan = true
        guard central.state == .poweredOn else { return }
        if case .ready = connection { return }
        discovered.removeAll()
        connection = .scanning
        central.scanForPeripherals(withServices: [serviceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

    func stopScan() {
        wantsScan = false
        if central.isScanning { central.stopScan() }
        if connection == .scanning { connection = .idle }
    }

    func connect(_ device: Discovered) {
        guard let p = peripherals[device.id] else { return }
        wantsScan = false
        central.stopScan()
        peripheral = p
        p.delegate = self
        connection = .connecting(device.name)
        central.connect(p)
    }

    func disconnect() {
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
    }

    /// Leave the installer and boot the installed application.
    func startInstalledApp() {
        write(InstallerProtocol.command(.reboot), to: controlChar)
    }

    // MARK: - Wi-Fi

    var supportsWifi: Bool { wifiChar != nil }

    /// Replace the device's saved networks. The device answers through the WIFI
    /// characteristic, which is polled until its operation counter moves.
    func saveWifi(_ networks: [WifiNetwork]) {
        guard isReady, wifiChar != nil, install?.isActive != true, wifiSync != .saving else { return }
        if let problem = networks.lazy.compactMap(\.problem).first {
            wifiSync = .failed(problem)
            return
        }
        wifiOpsBefore = deviceWifi?.operations ?? 0
        wifiSync = .saving
        write(InstallerProtocol.wifiSet(Array(networks.prefix(WifiNetwork.maxCount))), to: controlChar)
        let started = Date()
        wifiPoll?.invalidate()
        wifiPoll = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                if self.wifiSync != .saving {
                    self.stopWifiPoll()
                } else if Date().timeIntervalSince(started) > 15 {
                    self.stopWifiPoll()
                    self.wifiSync = .failed("设备没有回应")
                } else if let peripheral = self.peripheral, let wifiChar = self.wifiChar {
                    peripheral.readValue(for: wifiChar)
                }
            }
        }
    }

    func dismissWifiResult() {
        if wifiSync != .saving { wifiSync = .idle }
    }

    private func stopWifiPoll() {
        wifiPoll?.invalidate()
        wifiPoll = nil
    }

    private func handle(wifi list: InstallerWifiList) {
        deviceWifi = list
        guard wifiSync == .saving, list.operations != wifiOpsBefore else { return }
        stopWifiPoll()
        wifiSync = list.result == .saved ? .saved(applied: list.applied) : .failed(list.result.message)
    }

    // MARK: - Install

    func beginInstall(image: FirmwareImage, name: String) {
        guard isReady, let peripheral, controlChar != nil, dataChar != nil else { return }
        guard image.compatible else {
            install = InstallState(name: name, phase: .failed(image.tableError?.message ?? "不兼容"),
                                   total: image.dataEnd - InstallerLayout.streamStart)
            return
        }
        let maxWrite = min(peripheral.maximumWriteValueLength(for: .withoutResponse), 512)
        planner = TransferPlanner(image: image, maxPayload: maxWrite - InstallerProtocol.dataHeader)
        pendingImage = image
        commitSent = false
        rateMark = nil
        install = InstallState(name: name, phase: .starting, total: image.dataEnd - InstallerLayout.streamStart)
        UIApplication.shared.isIdleTimerDisabled = true
        write(InstallerProtocol.begin(end: image.dataEnd, streamSHA256: image.streamSHA256, name: name),
              to: controlChar)
        startWatchdog()
    }

    /// Resume after a reconnect: the device keeps the session in memory.
    func resumeInstall() {
        guard let image = pendingImage, let state = install else { return }
        beginInstall(image: image, name: state.name)
    }

    var canResume: Bool {
        guard let install, pendingImage != nil else { return false }
        if case .failed = install.phase { return isReady }
        return false
    }

    func cancelInstall() {
        if install?.isActive == true { write(InstallerProtocol.command(.abort), to: controlChar) }
        finishInstall()
        install = nil
        pendingImage = nil
    }

    func dismissInstall() {
        if install?.isActive == true { return }
        install = nil
        pendingImage = nil
    }

    private func finishInstall() {
        planner = nil
        watchdog?.invalidate()
        watchdog = nil
        UIApplication.shared.isIdleTimerDisabled = false
    }

    private func fail(_ message: String) {
        guard var state = install else { return }
        state.phase = .failed(message)
        install = state
        finishInstall()
    }

    private func pump() {
        guard let peripheral, let dataChar, install?.phase == .transferring else { return }
        while peripheral.canSendWriteWithoutResponse, let packet = planner?.nextPacket() {
            peripheral.writeValue(packet, for: dataChar, type: .withoutResponse)
        }
    }

    private func handle(status: InstallerStatus) {
        guard install != nil else { return }
        if status.sequence <= lastSequence && status.sequence != 0 { return }
        lastSequence = status.sequence
        switch status.phase {
        case .receiving:
            guard var state = install, planner != nil else { return }
            if state.phase == .starting {
                state.phase = .transferring
                state.resumed = status.resumed
                planner?.rewind(to: status.position)
            } else if status.resync {
                planner?.rewind(to: status.position)
            } else {
                planner?.acknowledge(status.position)
            }
            lastProgressAt = Date()
            updateRate(&state, position: status.position)
            state.sent = status.position - InstallerLayout.streamStart
            install = state
            if planner?.isComplete == true && !commitSent {
                commitSent = true
                write(InstallerProtocol.command(.commit), to: controlChar)
            }
            pump()
        case .verifying:
            install?.phase = .verifying
            install?.sent = install?.total ?? 0
        case .done:
            install?.phase = .done
            pendingImage = nil
            finishInstall()
        case .failed:
            let message = status.error == .table ? status.tableError.message : status.error.message
            fail(message)
        case .idle:
            if install?.isActive == true { fail("设备已取消安装") }
        }
    }

    private func updateRate(_ state: inout InstallState, position: UInt32) {
        let now = Date()
        guard let mark = rateMark else {
            rateMark = (now, position)
            return
        }
        let dt = now.timeIntervalSince(mark.time)
        guard dt >= 1, position >= mark.position else { return }
        let rate = Double(position - mark.position) / dt
        state.bytesPerSecond = state.bytesPerSecond == 0 ? rate : state.bytesPerSecond * 0.6 + rate * 0.4
        rateMark = (now, position)
    }

    private func startWatchdog() {
        watchdog?.invalidate()
        lastProgressAt = Date()
        watchdog = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.checkProgress() }
        }
    }

    private func checkProgress() {
        guard let state = install, state.phase == .transferring || state.phase == .starting else { return }
        let idle = Date().timeIntervalSince(lastProgressAt)
        if idle > 30 {
            fail("设备长时间没有响应")
        } else if idle > 3, let planner, !planner.isComplete {
            // Lost packets at the end of the window: resend from the last ack.
            self.planner?.rewind(to: planner.ackedPosition)
            lastProgressAt = Date()
            pump()
        }
    }

    private func write(_ data: Data, to characteristic: CBCharacteristic?) {
        guard let peripheral, let characteristic else { return }
        peripheral.writeValue(data, for: characteristic, type: .withResponse)
    }

    private func resetLink() {
        peripheral = nil
        infoChar = nil
        controlChar = nil
        dataChar = nil
        statusChar = nil
        wifiChar = nil
        deviceWifi = nil
        stopWifiPoll()
        if wifiSync == .saving { wifiSync = .failed("连接断开") }
        info = nil
        lastSequence = 0
    }
}

// MARK: - CBCentralManagerDelegate

extension DeviceManager: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        MainActor.assumeIsolated {
            switch central.state {
            case .poweredOn:
                if connection == .bluetoothOff || connection == .unauthorized { connection = .idle }
                if wantsScan { startScan() }
            case .unauthorized:
                connection = .unauthorized
            case .poweredOff, .unsupported, .resetting, .unknown:
                connection = .bluetoothOff
            @unknown default:
                connection = .bluetoothOff
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any], rssi RSSI: NSNumber) {
        MainActor.assumeIsolated {
            let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? "Passport"
            peripherals[peripheral.identifier] = peripheral
            let entry = Discovered(id: peripheral.identifier, name: name, rssi: RSSI.intValue)
            if let index = discovered.firstIndex(where: { $0.id == entry.id }) {
                discovered[index] = entry
            } else {
                discovered.append(entry)
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated {
            peripheral.discoverServices([serviceUUID])
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            resetLink()
            connection = .failed("连接失败：\(error?.localizedDescription ?? "未知错误")")
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            resetLink()
            connection = .idle
            if let state = install {
                switch state.phase {
                case .done:
                    break   // the device restarts into the new application
                case .verifying:
                    fail("校验时连接断开；请重新连接查看结果")
                case .starting, .transferring:
                    fail("连接断开。重新连接后可以从断点继续")
                case .failed:
                    break
                }
            }
        }
    }
}

// MARK: - CBPeripheralDelegate

extension DeviceManager: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated {
            guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else {
                connection = .failed("这台设备没有运行安装器")
                central.cancelPeripheralConnection(peripheral)
                return
            }
            peripheral.discoverCharacteristics(nil, for: service)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                                error: Error?) {
        MainActor.assumeIsolated {
            for c in service.characteristics ?? [] {
                switch c.uuid.uuidString.uppercased() {
                case InstallerUUID.info: infoChar = c
                case InstallerUUID.control: controlChar = c
                case InstallerUUID.data: dataChar = c
                case InstallerUUID.status: statusChar = c
                case InstallerUUID.wifi: wifiChar = c
                default: break
                }
            }
            guard let infoChar, controlChar != nil, dataChar != nil, statusChar != nil else {
                connection = .failed("安装器服务不完整")
                return
            }
            if case .connecting(let name) = connection { connection = .pairing(name) }
            // The first encrypted read makes iOS ask for the passkey on the device screen.
            peripheral.readValue(for: infoChar)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated {
            if let error {
                if characteristic == infoChar {
                    connection = .failed("配对失败或已取消（\(error.localizedDescription)）")
                    central.cancelPeripheralConnection(peripheral)
                }
                return
            }
            guard let value = characteristic.value else { return }
            if characteristic == infoChar {
                guard let parsed = InstallerInfo(value), parsed.protocolVersion == InstallerProtocol.version else {
                    connection = .failed("安装器版本与 App 不匹配，请更新")
                    return
                }
                info = parsed
                if let statusChar { peripheral.setNotifyValue(true, for: statusChar) }
                if let wifiChar { peripheral.readValue(for: wifiChar) }
                if case .pairing(let name) = connection { connection = .ready(name) }
            } else if characteristic == statusChar, let status = InstallerStatus(value) {
                handle(status: status)
            } else if characteristic == wifiChar, let list = InstallerWifiList(value) {
                handle(wifi: list)
            }
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated {
            if let error, characteristic == controlChar, install?.isActive == true {
                fail("设备拒绝了命令：\(error.localizedDescription)")
            }
            if let error, characteristic == controlChar, wifiSync == .saving {
                stopWifiPoll()
                wifiSync = .failed("设备拒绝了命令：\(error.localizedDescription)")
            }
        }
    }

    nonisolated func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        MainActor.assumeIsolated { pump() }
    }
}
