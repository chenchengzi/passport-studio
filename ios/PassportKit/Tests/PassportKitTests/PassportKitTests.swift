import CryptoKit
import XCTest
@testable import PassportKit

// MARK: - Fixtures

func tableEntry(_ type: UInt8, _ subtype: UInt8, _ offset: UInt32, _ size: UInt32, _ label: String,
                flags: UInt32 = 0) -> Data {
    var e = Data([0xAA, 0x50, type, subtype])
    e.appendLE(offset)
    e.appendLE(size)
    var name = Data(label.utf8)
    name.append(Data(repeating: 0, count: 16 - name.count))
    e.append(name)
    e.appendLE(flags)
    return e
}

func table(_ entries: [Data], md5: Bool = true) -> Data {
    var t = entries.reduce(Data(), +)
    if md5 {
        var marker = Data([0xEB, 0xEB])
        marker.append(Data(repeating: 0xFF, count: 14))
        marker.append(Data(Insecure.MD5.hash(data: t)))
        t.append(marker)
    }
    return t
}

/// Merged image: bootloader stub, the given table, app at 0x10000, 0xFF holes.
func mergedImage(table t: Data, appSize: Int = 0x30000, padTo: Int? = nil) -> Data {
    var image = Data(repeating: 0xFF, count: 0x10000 + appSize)
    image[0] = 0xE9
    image[12] = 5
    image.replaceSubrange(0x8000..<(0x8000 + t.count), with: t)
    for i in 0x10000..<image.count { image[i] = UInt8(truncatingIfNeeded: i &* 7 &+ 3) }
    image[0x10000] = 0xE9
    image.replaceSubrange(0x20000..<0x23000, with: Data(repeating: 0xFF, count: 0x3000))
    if let padTo, padTo > image.count { image.append(Data(repeating: 0xFF, count: padTo - image.count)) }
    return image
}

let defaultEntries = [
    tableEntry(1, 0x02, 0x9000, 0x6000, "nvs"),
    tableEntry(1, 0x01, 0xF000, 0x1000, "phy_init"),
    tableEntry(0, 0x00, 0x10000, 0x7F0000, "factory"),
]

/// Same position rules as inst_session.c, enough to exercise the sender.
struct SimulatedDevice {
    var position = InstallerLayout.streamStart
    var end: UInt32
    var flash: [UInt8]

    init(end: UInt32) {
        self.end = end
        flash = [UInt8](repeating: 0x00, count: Int(end))
    }

    /// Returns the position the device would report, or nil for an in-order packet.
    mutating func receive(_ packet: Data) -> UInt32? {
        let b = [UInt8](packet)
        let offset = b.le32(1)
        guard offset == position else { return position }
        if b[0] == InstallerOp.data.rawValue {
            let payload = b[5...]
            guard position + UInt32(payload.count) <= end else { return position }
            flash.replaceSubrange(Int(position)..<Int(position) + payload.count, with: payload)
            position += UInt32(payload.count)
        } else {
            let to = b.le32(5)
            guard to > position, to <= end else { return position }
            for i in Int(position)..<Int(to) { flash[i] = 0xFF }
            position = to
        }
        return nil
    }
}

// MARK: - Tests

final class FirmwareImageTests: XCTestCase {
    func testDefaultTemplateIsCompatible() throws {
        let image = try FirmwareImage(fileData: mergedImage(table: table(defaultEntries), padTo: 0x800000))
        XCTAssertEqual(image.kind, .merged)
        XCTAssertNil(image.tableError)
        XCTAssertEqual(image.dataEnd, 0x40000)
        XCTAssertEqual(image.partitions.map(\.label), ["nvs", "phy_init", "factory"])
        XCTAssertEqual(image.bootPartition?.offset, 0x10000)
        let expected = Data(SHA256.hash(data: image.bytes[0x8000..<0x40000]))
        XCTAssertEqual(image.streamSHA256, expected)
    }

    func testRejections() throws {
        func error(_ entries: [Data], appSize: Int = 0x30000, md5: Bool = true) throws -> TableError? {
            try FirmwareImage(fileData: mergedImage(table: table(entries, md5: md5), appSize: appSize)).tableError
        }
        XCTAssertEqual(try error(defaultEntries, appSize: Int(InstallerLayout.playLimit) - 0x10000 + 1), .imageEnd)
        XCTAssertNil(try error(defaultEntries, appSize: Int(InstallerLayout.playLimit) - 0x10000))
        XCTAssertEqual(try error([tableEntry(1, 2, 0x9000, 0x6000, "nvs"),
                                  tableEntry(0, 0, 0x10000, 0x300000, "factory"),
                                  tableEntry(1, 0x82, 0x700000, 0x100000, "storage")]), .reserved)
        XCTAssertNil(try error([tableEntry(0, 0, 0x10000, 0x300000, "factory"),
                                tableEntry(1, 0x82, 0x310000, 0x4F0000, "storage")]))
        XCTAssertEqual(try error([tableEntry(0, 0, 0x10000, 0x300000, "factory"),
                                  tableEntry(1, 0x82, 0x300000, 0x10000, "storage")]), .overlap)
        XCTAssertEqual(try error([tableEntry(0, 0, 0x10000, 0x300000, "factory", flags: 1)]), .entry)
        XCTAssertEqual(try error([tableEntry(1, 2, 0x9000, 0x6000, "nvs")]), .noApp)
        XCTAssertEqual(try error([tableEntry(0, 0, 0x10000, 0x300000, "factory"),
                                  tableEntry(0, 0x20, 0x310000, 0x100000, "test")]), .reserved)
        XCTAssertNil(try error([tableEntry(0, 0, 0x10000, 0x300000, "factory")], md5: false))

        var corrupt = table(defaultEntries)
        corrupt[70] ^= 0xFF
        XCTAssertEqual(try FirmwareImage(fileData: mergedImage(table: corrupt)).tableError, .format)
    }

    func testRepositoryTableWithInstallerEntriesIsAccepted() throws {
        let entries = [
            tableEntry(1, 0x02, 0x9000, 0x6000, "nvs"),
            tableEntry(1, 0x01, 0xF000, 0x1000, "phy_init"),
            tableEntry(0, 0x00, 0x10000, InstallerLayout.playLimit - 0x10000, "factory"),
            tableEntry(1, 0x40, InstallerLayout.stateOffset, InstallerLayout.stateSize, "inst_state"),
            tableEntry(0, 0x20, InstallerLayout.appOffset, InstallerLayout.appSize, "installer"),
            tableEntry(1, 0x41, InstallerLayout.wifiOffset, InstallerLayout.wifiSize, "inst_wifi"),
        ]
        XCTAssertNil(try FirmwareImage(fileData: mergedImage(table: table(entries))).tableError)
        // Earlier template revision: 1 MB installer, no Wi-Fi sector.
        var old = Array(entries.prefix(4))
        old.append(tableEntry(0, 0x20, InstallerLayout.appOffset, 0x100000, "installer"))
        XCTAssertNil(try FirmwareImage(fileData: mergedImage(table: table(old))).tableError)
        // The Wi-Fi label anywhere else is reserved.
        XCTAssertEqual(try FirmwareImage(fileData: mergedImage(table: table([
            tableEntry(0, 0x00, 0x10000, 0x300000, "factory"),
            tableEntry(1, 0x82, 0x500000, 0x1000, "inst_wifi")]))).tableError, .reserved)
    }

    func testWifiPacketsAndList() {
        XCTAssertNil(WifiNetwork(ssid: "Home", password: "password1").problem)
        XCTAssertNil(WifiNetwork(ssid: "Open", password: "").problem)
        XCTAssertNotNil(WifiNetwork(ssid: "", password: "").problem)
        XCTAssertNotNil(WifiNetwork(ssid: "Home", password: "short").problem)
        XCTAssertNotNil(WifiNetwork(ssid: String(repeating: "a", count: 33), password: "").problem)
        XCTAssertNotNil(WifiNetwork(ssid: "Home", password: String(repeating: "g", count: 64)).problem)
        XCTAssertNil(WifiNetwork(ssid: "Home", password: String(repeating: "a", count: 64)).problem)

        let packet = InstallerProtocol.wifiSet([WifiNetwork(ssid: "Home", password: "password1"),
                                                WifiNetwork(ssid: "Lab", password: "")])
        XCTAssertEqual([UInt8](packet), [0x20, 2, 4] + Array("Home".utf8) + [9] + Array("password1".utf8)
                       + [3] + Array("Lab".utf8) + [0])

        let list = InstallerWifiList(Data([1, 1, 0x01, 2, 4, 3, 2, 1, 4, 72, 111, 109, 101, 3, 76, 97, 98]))
        XCTAssertEqual(list?.result, .saved)
        XCTAssertEqual(list?.applied, true)
        XCTAssertEqual(list?.operations, 0x01020304)
        XCTAssertEqual(list?.ssids, ["Home", "Lab"])
        XCTAssertNil(InstallerWifiList(Data([1, 1, 0, 1, 0, 0, 0, 0, 5, 65])))   // truncated
    }

    func testApplicationOnlyImageIsWrapped() throws {
        var app = Data(repeating: 0x5A, count: 0x20000)
        app[0] = 0xE9
        app[12] = 5
        app[13] = 0
        let image = try FirmwareImage(fileData: app)
        XCTAssertEqual(image.kind, .applicationOnly)
        XCTAssertNil(image.tableError)
        XCTAssertEqual(image.bytes[0x10000..<0x30000], app[0..<0x20000])
        XCTAssertEqual(image.partitions.last?.size, InstallerLayout.playLimit - 0x10000)

        app[12] = 2   // ESP32-S2
        XCTAssertThrowsError(try FirmwareImage(fileData: app)) { error in
            XCTAssertEqual(error as? FirmwareImageError, .notESP32C3)
        }
        XCTAssertThrowsError(try FirmwareImage(fileData: Data(repeating: 0x00, count: 0x1000)))
    }
}

final class TransferPlannerTests: XCTestCase {
    func sendAll(image: FirmwareImage, payload: Int, dropEvery: Int, resumeAt: UInt32? = nil) -> SimulatedDevice {
        var planner = TransferPlanner(image: image, maxPayload: payload)
        var device = SimulatedDevice(end: image.dataEnd)
        var sent = 0
        var stalls = 0
        while !planner.isComplete {
            guard let packet = planner.nextPacket() else {
                // Window full or everything sent: the device acknowledges what it has.
                if device.position == planner.ackedPosition { stalls += 1 }
                XCTAssertLessThan(stalls, 10_000)
                // A timeout in the app rewinds to the device position.
                planner.rewind(to: device.position)
                continue
            }
            sent += 1
            if let resumeAt, device.position >= resumeAt, planner.ackedPosition < resumeAt {
                // Disconnect: the planner restarts from what the device reports.
                planner = TransferPlanner(image: image, maxPayload: payload, startPosition: device.position)
                continue
            }
            if dropEvery > 0 && sent % dropEvery == 0 { continue }
            if let expected = device.receive(packet) {
                planner.rewind(to: expected)
            } else if device.position - planner.ackedPosition >= UInt32(InstallerProtocol.ackInterval) ||
                        device.position == image.dataEnd {
                planner.acknowledge(device.position)
            }
            XCTAssertLessThanOrEqual(planner.bytesInFlight, InstallerProtocol.windowBytes + payload + 16)
        }
        return device
    }

    func testStreamsImageWithSkipsAndLoss() throws {
        let image = try FirmwareImage(fileData: mergedImage(table: table(defaultEntries), appSize: 0x60000))
        for (payload, drop) in [(180, 0), (495, 0), (244, 7), (512, 3)] {
            let device = sendAll(image: image, payload: payload, dropEvery: drop)
            XCTAssertEqual(device.position, image.dataEnd)
            XCTAssertEqual(Data(device.flash[0x8000...]), image.stream, "payload \(payload) drop \(drop)")
        }
        let planner = TransferPlanner(image: image, maxPayload: 244)
        XCTAssertLessThan(planner.payloadBytes, Int(image.dataEnd) - 0x8000)   // holes are skipped
    }

    func testResumeFromDevicePosition() throws {
        let image = try FirmwareImage(fileData: mergedImage(table: table(defaultEntries), appSize: 0x40000))
        let device = sendAll(image: image, payload: 244, dropEvery: 0, resumeAt: 0x22000)
        XCTAssertEqual(Data(device.flash[0x8000...]), image.stream)
    }

    func testPacketEncoding() {
        let sha = Data(repeating: 0xAB, count: 32)
        let begin = InstallerProtocol.begin(end: 0x123456, streamSHA256: sha, name: "番茄钟 Pomodoro")
        XCTAssertEqual(begin[0], 0x01)
        XCTAssertEqual([UInt8](begin).le32(1), 0x123456)
        XCTAssertEqual(begin[5..<37], sha)
        XCTAssertEqual(Int(begin[37]), begin.count - 38)
        let long = InstallerProtocol.begin(end: 1, streamSHA256: sha, name: String(repeating: "长", count: 40))
        XCTAssertEqual(long[37], 63)   // 21 three-byte characters, never split
        XCTAssertEqual(String(data: long[38...], encoding: .utf8), String(repeating: "长", count: 21))

        XCTAssertEqual([UInt8](InstallerProtocol.skip(offset: 0x9000, to: 0x10000)),
                       [0x11, 0x00, 0x90, 0, 0, 0x00, 0x00, 0x01, 0])
    }

    func testStatusAndInfoDecoding() {
        var status = Data([3, 0, 0, 0x03])
        status.appendLE(0x40000)
        status.appendLE(0x40000)
        status.appendLE(7)
        let s = InstallerStatus(status)
        XCTAssertEqual(s?.phase, .done)
        XCTAssertEqual(s?.resumed, true)
        XCTAssertEqual(s?.resync, true)
        XCTAssertEqual(s?.position, 0x40000)
        XCTAssertNil(InstallerStatus(Data([1, 2, 3])))

        var info = Data([1, 0x05, 0, 0])
        info.appendLE(0x8000)
        info.appendLE(InstallerLayout.playLimit)
        info.appendLE(0x40000)
        info.append(Data(repeating: 0x11, count: 32))
        let name = Data("番茄钟".utf8)
        info.append(UInt8(name.count))
        info.append(name)
        info.append(5)
        info.append(Data("1.0.0".utf8))
        let i = InstallerInfo(info)
        XCTAssertEqual(i?.hasRecord, true)
        XCTAssertEqual(i?.interrupted, false)
        XCTAssertEqual(i?.appPresent, true)
        XCTAssertEqual(i?.installedName, "番茄钟")
        XCTAssertEqual(i?.firmwareVersion, "1.0.0")
        XCTAssertEqual(i?.playLimit, InstallerLayout.playLimit)
        XCTAssertNil(InstallerInfo(info.prefix(60)))
    }
}

final class CommunityTests: XCTestCase {
    func testLinkParsing() {
        XCTAssertEqual(PlayReference.parse("https://ai-passport.folotoy.cn/plays/?play=community-f4a880d1"),
                       .slug("community-f4a880d1"))
        XCTAssertEqual(PlayReference.parse("快来玩 https://ai-passport.folotoy.cn/plays/?play=pocket-tetris 很好玩"),
                       .slug("pocket-tetris"))
        XCTAssertEqual(PlayReference.parse("https://ai-passport.folotoy.cn/plays/xiaozhi-ai-chatbot"),
                       .slug("xiaozhi-ai-chatbot"))
        XCTAssertEqual(PlayReference.parse("https://ai-passport.folotoy.cn/api/plays/id/2494"), .id(2494))
        XCTAssertEqual(PlayReference.parse(
            "https://ai-passport.folotoy.cn/plays/community/2174/vibe?mp_flow=x&utm_source=wechat_miniapp"), .id(2174))
        XCTAssertEqual(PlayReference.parse("https://ai-passport.folotoy.cn/plays/2174/?utm_medium=app"), .id(2174))
        XCTAssertEqual(PlayReference.parse("https://apsimulator.folotoy.cn/?play=2494"), .id(2494))
        XCTAssertEqual(PlayReference.parse("  community-52fee465 "), .slug("community-52fee465"))
        XCTAssertEqual(PlayReference.parse("2494"), .id(2494))
        XCTAssertNil(PlayReference.parse("https://example.com/plays/?play=evil"))
        XCTAssertNil(PlayReference.parse("hello world"))
        XCTAssertNil(PlayReference.parse("../etc/passwd"))
        XCTAssertEqual(CommunityAPI.metadataURL(for: .slug("pocket-tetris")).absoluteString,
                       "https://ai-passport.folotoy.cn/api/plays/pocket-tetris")
        XCTAssertEqual(CommunityAPI.metadataURL(for: .id(12)).absoluteString,
                       "https://ai-passport.folotoy.cn/api/plays/id/12")
    }

    func testDecodeAndVerify() throws {
        let body = Data("ok-firmware".utf8)
        let sha = Data(SHA256.hash(data: body)).hexString
        let json = """
        {"ok":true,"play":{"id":2494,"slug":"community-f4a880d1","title":{"zh":"铃声回响","en":"Ring Echo"},
        "author":"split","shareVersion":"6242-2","thumbnail":"/api/media/x.webp","extra":1,
        "firmware":{"available":true,"size":\(body.count),"sha256":"\(sha)",
        "url":"/api/download/community/community-f4a880d1","format":"esp-merged-0x0"}}}
        """
        let play = try CommunityAPI.decodePlay(Data(json.utf8))
        XCTAssertEqual(play.title.preferred, "铃声回响")
        XCTAssertEqual(play.shareLink.absoluteString, "https://ai-passport.folotoy.cn/plays/?play=community-f4a880d1")
        XCTAssertEqual(try CommunityAPI.firmwareURL(for: play).absoluteString,
                       "https://ai-passport.folotoy.cn/api/download/community/community-f4a880d1")
        XCTAssertNoThrow(try CommunityAPI.verify(body, for: play))
        XCTAssertThrowsError(try CommunityAPI.verify(Data("tampered!!!".utf8), for: play))

        var offsite = play
        offsite.firmware?.url = "https://evil.example/fw.bin"
        XCTAssertThrowsError(try CommunityAPI.firmwareURL(for: offsite)) { error in
            XCTAssertEqual(error as? CommunityError, .unsafeURL)
        }
        var other = play
        other.firmware?.format = "esp-app-0x10000"
        XCTAssertThrowsError(try CommunityAPI.firmwareURL(for: other))
        XCTAssertThrowsError(try CommunityAPI.decodePlay(Data(#"{"detail":"玩法不存在"}"#.utf8)))
    }
}

final class LibraryTests: XCTestCase {
    func testAddDeduplicateRemove() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: dir) }
        let library = try FirmwareLibrary(directory: dir)
        let data = mergedImage(table: table(defaultEntries))
        let a = try library.add(data: data, name: "我的番茄钟", source: .local(fileName: "full.bin"))
        XCTAssertNil(a.compatibilityError)
        let again = try library.add(data: data, name: "copy", source: .local(fileName: "other.bin"))
        XCTAssertEqual(again.id, a.id)
        XCTAssertEqual(library.items.count, 1)

        // Community items update in place when a new version arrives.
        let c1 = try library.add(data: mergedImage(table: table(defaultEntries), appSize: 0x20000),
                                 name: "俄罗斯方块", source: .community(slug: "pocket-tetris", playID: 1,
                                                                   author: "a", shareVersion: "1-1"))
        let c2 = try library.add(data: mergedImage(table: table(defaultEntries), appSize: 0x28000),
                                 name: "俄罗斯方块", source: .community(slug: "pocket-tetris", playID: 1,
                                                                   author: "a", shareVersion: "2-1"))
        XCTAssertEqual(c1.id, c2.id)
        XCTAssertNotEqual(c1.fileSHA256, c2.fileSHA256)
        XCTAssertEqual(library.items.count, 2)

        let reloaded = try FirmwareLibrary(directory: dir)
        XCTAssertEqual(reloaded.items, library.items)
        let image = try reloaded.image(for: a)
        XCTAssertEqual(reloaded.item(matchingStreamSHA256: image.streamSHA256)?.id, a.id)

        try reloaded.remove(a)
        XCTAssertEqual(reloaded.items.count, 1)
        XCTAssertThrowsError(try reloaded.data(for: a))
        XCTAssertThrowsError(try library.add(data: Data(repeating: 0, count: 100), name: "x",
                                             source: .local(fileName: "x.bin")))
    }
}

/// Set PASSPORT_SAMPLE_FIRMWARE to a real merged image to check it end to end.
final class SampleFirmwareTests: XCTestCase {
    func testSampleFirmware() throws {
        guard let path = ProcessInfo.processInfo.environment["PASSPORT_SAMPLE_FIRMWARE"] else {
            throw XCTSkip("PASSPORT_SAMPLE_FIRMWARE not set")
        }
        let data = try Data(contentsOf: URL(fileURLWithPath: path))
        let image = try FirmwareImage(fileData: data)
        print("sample: \(data.count) bytes, data end 0x\(String(image.dataEnd, radix: 16)), " +
              "table: \(image.tableError?.message ?? "compatible")")
        var planner = TransferPlanner(image: image, maxPayload: 495)
        print("sample: \(planner.payloadBytes) bytes over the air")
        var device = SimulatedDevice(end: image.dataEnd)
        while !planner.isComplete {
            guard let packet = planner.nextPacket() else {
                planner.acknowledge(device.position)
                continue
            }
            if let expected = device.receive(packet) { planner.rewind(to: expected) }
        }
        XCTAssertEqual(Data(device.flash[0x8000...]), image.stream)
    }
}

/// Set PASSPORT_SAMPLE_PLAY_JSON to saved /api/plays/<slug> responses (colon separated).
final class SamplePlayTests: XCTestCase {
    func testSamplePlayJSON() throws {
        guard let paths = ProcessInfo.processInfo.environment["PASSPORT_SAMPLE_PLAY_JSON"] else {
            throw XCTSkip("PASSPORT_SAMPLE_PLAY_JSON not set")
        }
        for path in paths.split(separator: ":") {
            let play = try CommunityAPI.decodePlay(Data(contentsOf: URL(fileURLWithPath: String(path))))
            let url = try CommunityAPI.firmwareURL(for: play)
            print("play: \(play.slug) \(play.title.preferred) by \(play.author ?? "-") -> \(url.path), " +
                  "\(play.firmware?.size ?? 0) bytes")
        }
    }
}
