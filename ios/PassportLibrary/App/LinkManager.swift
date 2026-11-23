import CoreBluetooth
import Foundation
import Observation
import PassportKit

/// One connected (or remembered) Passport Link application.
@MainActor
@Observable
final class LinkSession: Identifiable {
    enum State: Equatable {
        case offline
        case connecting
        case pairing
        case ready
        case failed(String)
    }

    let id: UUID
    var name: String
    var state: State = .offline
    var info: LinkInfo?
    /// Values confirmed by the device.
    var values: [String: LinkValue] = [:]
    var lastError: String?
    var lastLocationName: String?
    var lastTimeSync: Date?

    fileprivate var peripheral: CBPeripheral
    fileprivate var infoChar: CBCharacteristic?
    fileprivate var rxChar: CBCharacteristic?
    fileprivate var txChar: CBCharacteristic?
    fileprivate var reassembler = LinkReassembler()

    init(peripheral: CBPeripheral, name: String) {
        self.id = peripheral.identifier
        self.peripheral = peripheral
        self.name = name
    }

    func uses(_ channel: String) -> Bool { info?.channels.contains(channel) ?? false }

    func value(for setting: LinkSetting) -> LinkValue {
        values[setting.key] ?? setting.defaultValue
    }
}

/// CoreBluetooth side of Passport Link. Remembered devices are reconnected
/// automatically whenever they are in range; iOS keeps the pending connection
/// alive while the app is suspended (bluetooth-central background mode).
@MainActor
@Observable
final class LinkManager: NSObject {
    struct Nearby: Identifiable, Equatable {
        let id: UUID
        var name: String
        var rssi: Int
    }

    private(set) var bluetoothReady = false
    private(set) var nearby: [Nearby] = []
    private(set) var sessions: [UUID: LinkSession] = [:]
    let location = LocationProvider()

    private var central: CBCentralManager!
    private var discoveredPeripherals: [UUID: CBPeripheral] = [:]
    private var scanning = false

    private let serviceUUID = CBUUID(string: LinkUUID.service)
    private static let rememberedKey = "link.remembered"   // [uuid: name]

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
        location.onChange = { [weak self] in self?.pushLocationToAll() }
    }

    /// Remembered first, by name.
    var orderedSessions: [LinkSession] {
        sessions.values.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    private var remembered: [String: String] {
        get { UserDefaults.standard.dictionary(forKey: Self.rememberedKey) as? [String: String] ?? [:] }
        set { UserDefaults.standard.set(newValue, forKey: Self.rememberedKey) }
    }

    // MARK: - Scanning and connection

    func startScan() {
        scanning = true
        guard bluetoothReady else { return }
        nearby.removeAll()
        central.scanForPeripherals(withServices: [serviceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    func stopScan() {
        scanning = false
        if central.isScanning { central.stopScan() }
    }

    func connect(_ device: Nearby) {
        guard let p = discoveredPeripherals[device.id] else { return }
        let session = sessions[p.identifier] ?? LinkSession(peripheral: p, name: device.name)
        sessions[p.identifier] = session
        open(session)
    }

    func forget(_ session: LinkSession) {
        var r = remembered
        r.removeValue(forKey: session.id.uuidString)
        remembered = r
        central.cancelPeripheralConnection(session.peripheral)
        sessions.removeValue(forKey: session.id)
    }

    func reconnect(_ session: LinkSession) {
        if session.state == .ready { return }
        open(session)
    }

    private func open(_ session: LinkSession) {
        session.peripheral.delegate = self
        session.state = .connecting
        session.lastError = nil
        central.connect(session.peripheral)   // pending until in range; no timeout
    }

    private func restoreRemembered() {
        let ids = remembered.keys.compactMap(UUID.init(uuidString:))
        for p in central.retrievePeripherals(withIdentifiers: ids) where sessions[p.identifier] == nil {
            let session = LinkSession(peripheral: p, name: remembered[p.identifier.uuidString] ?? "Passport")
            sessions[p.identifier] = session
            open(session)
        }
    }

    // MARK: - Messages

    func set(_ session: LinkSession, key: String, value: LinkValue) {
        guard let setting = session.info?.settings.first(where: { $0.key == key }), setting.accepts(value) else {
            session.lastError = "值不符合设置要求"
            return
        }
        send(.set([key: value]), to: session)
    }

    func pushLocation(to session: LinkSession) {
        guard session.uses("location") else { return }
        guard let fix = location.current else {
            location.request()
            return
        }
        send(.location(latitude: fix.latitude, longitude: fix.longitude, accuracy: fix.accuracy, name: fix.name,
                       nameZh: fix.nameZh), to: session)
        session.lastLocationName = fix.name
    }

    private func pushLocationToAll() {
        for session in sessions.values where session.state == .ready { pushLocation(to: session) }
    }

    private func pushTime(to session: LinkSession) {
        guard session.uses("time") else { return }
        send(.time(), to: session)
        session.lastTimeSync = Date()
    }

    private func send(_ message: LinkOutgoing, to session: LinkSession) {
        guard session.state == .ready, let rx = session.rxChar else { return }
        let maxFrame = min(session.peripheral.maximumWriteValueLength(for: .withResponse), 512)
        for frame in LinkFrame.split(message.data, maxFrame: maxFrame) {
            session.peripheral.writeValue(frame, for: rx, type: .withResponse)
        }
    }

    private func handle(_ incoming: LinkIncoming, from session: LinkSession) {
        switch incoming {
        case .settings(let values):
            session.values = values
            session.lastError = nil
        case .error(let why, let key):
            session.lastError = key.map { "\($0)：\(why)" } ?? why
        case .request(let channel):
            if channel == "location" { pushLocation(to: session) }
            if channel == "time" { pushTime(to: session) }
        }
    }

    /// Called once the link is encrypted and TX is subscribed.
    private func didBecomeReady(_ session: LinkSession) {
        session.state = .ready
        var r = remembered
        r[session.id.uuidString] = session.name
        remembered = r
        send(.get, to: session)
        pushTime(to: session)
        if session.uses("location") {
            location.request()
            pushLocation(to: session)
        }
    }

    private func session(for peripheral: CBPeripheral) -> LinkSession? {
        sessions[peripheral.identifier]
    }
}

// MARK: - CBCentralManagerDelegate

extension LinkManager: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        MainActor.assumeIsolated {
            bluetoothReady = central.state == .poweredOn
            guard bluetoothReady else { return }
            restoreRemembered()
            if scanning { startScan() }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any], rssi RSSI: NSNumber) {
        MainActor.assumeIsolated {
            let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? "Passport"
            discoveredPeripherals[peripheral.identifier] = peripheral
            let entry = Nearby(id: peripheral.identifier, name: name, rssi: RSSI.intValue)
            if let i = nearby.firstIndex(where: { $0.id == entry.id }) { nearby[i] = entry } else { nearby.append(entry) }
            if let session = sessions[peripheral.identifier] { session.name = name }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated {
            session(for: peripheral)?.state = .pairing
            peripheral.discoverServices([serviceUUID])
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            session(for: peripheral)?.state = .failed(error?.localizedDescription ?? "连接失败")
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            guard let session = session(for: peripheral) else { return }
            session.state = .offline
            session.reassembler = LinkReassembler()
            // Keep remembered devices pending so they reconnect when back in range.
            if remembered[peripheral.identifier.uuidString] != nil { open(session) }
        }
    }
}

// MARK: - CBPeripheralDelegate

extension LinkManager: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated {
            guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else {
                session(for: peripheral)?.state = .failed("这台设备没有运行 Passport Link 应用")
                central.cancelPeripheralConnection(peripheral)
                return
            }
            peripheral.discoverCharacteristics(nil, for: service)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                                error: Error?) {
        MainActor.assumeIsolated {
            guard let session = session(for: peripheral) else { return }
            for c in service.characteristics ?? [] {
                switch c.uuid.uuidString.uppercased() {
                case LinkUUID.info: session.infoChar = c
                case LinkUUID.rx: session.rxChar = c
                case LinkUUID.tx: session.txChar = c
                default: break
                }
            }
            guard let info = session.infoChar, session.rxChar != nil, session.txChar != nil else {
                session.state = .failed("Link 服务不完整")
                return
            }
            // The first encrypted read starts pairing (only the first time).
            peripheral.readValue(for: info)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated {
            guard let session = session(for: peripheral) else { return }
            if let error {
                if characteristic == session.infoChar {
                    session.state = .failed("配对失败或已取消（\(error.localizedDescription)）")
                }
                return
            }
            guard let value = characteristic.value else { return }
            if characteristic == session.infoChar {
                do {
                    let info = try LinkInfo(value)
                    guard info.version == LinkFrame.protocolVersion else {
                        session.state = .failed("设备上的 Link 版本与 App 不匹配")
                        return
                    }
                    session.info = info
                    session.name = info.name
                    if let tx = session.txChar { peripheral.setNotifyValue(true, for: tx) }
                } catch {
                    session.state = .failed("无法读取应用描述")
                }
            } else if characteristic == session.txChar, let message = session.reassembler.feed(value),
                      let incoming = LinkIncoming(message) {
                handle(incoming, from: session)
            }
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral,
                                didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            guard let session = session(for: peripheral), characteristic == session.txChar else { return }
            if let error {
                session.state = .failed("订阅失败：\(error.localizedDescription)")
            } else if characteristic.isNotifying {
                didBecomeReady(session)
            }
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated {
            if let error, let session = session(for: peripheral) {
                session.lastError = "发送失败：\(error.localizedDescription)"
            }
        }
    }
}
