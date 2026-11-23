import XCTest
@testable import PassportKit

final class LinkProtocolTests: XCTestCase {
    func testSplitAndReassemble() {
        let message = Data(#"{"t":"push","ch":"location","d":{"lat":39.98,"lon":116.31}}"#.utf8)
        let frames = LinkFrame.split(message, maxFrame: 20)
        XCTAssertEqual(frames.count, (message.count + 18) / 19)
        XCTAssertEqual(frames.first?.first, LinkFrame.first)
        XCTAssertEqual(frames.last?.first.map { $0 & LinkFrame.last }, LinkFrame.last)
        XCTAssertTrue(frames.allSatisfy { $0.count <= 20 })

        var rx = LinkReassembler()
        var result: Data?
        for frame in frames { result = rx.feed(frame) }
        XCTAssertEqual(result, message)

        XCTAssertEqual(LinkFrame.split(Data("{}".utf8), maxFrame: 64), [Data([0x03]) + Data("{}".utf8)])
    }

    func testReassemblerRejectsBadSequences() {
        var rx = LinkReassembler()
        XCTAssertNil(rx.feed(Data([0x00, 0x41])))          // continuation without start
        XCTAssertNil(rx.feed(Data([0x80, 0x41])))          // unknown flag
        XCTAssertNil(rx.feed(Data()))
        XCTAssertNil(rx.feed(Data([0x03])))                // empty message
        XCTAssertNil(rx.feed(Data([0x01, 0x61])))          // partial ...
        XCTAssertEqual(rx.feed(Data([0x03, 0x6F, 0x6B])), Data("ok".utf8))   // ... replaced

        let big = Data(repeating: 0x78, count: LinkFrame.messageMax + 1)
        var last: Data?
        for frame in LinkFrame.split(big, maxFrame: 200) { last = rx.feed(frame) }
        XCTAssertNil(last)
        let exact = Data(repeating: 0x78, count: LinkFrame.messageMax)
        for frame in LinkFrame.split(exact, maxFrame: 200) { last = rx.feed(frame) }
        XCTAssertEqual(last, exact)
    }

    func testInfoAndSettings() throws {
        let json = #"""
        {"v":1,"id":"wifi-weather","name":"Passport Weather","ver":"1.1.0","ch":["location","time"],
         "set":[{"k":"units","n":"温度单位","type":"enum","def":"C","opt":["C","F"]},
                {"k":"refresh","n":"刷新间隔","type":"int","def":30,"min":5,"max":180},
                {"k":"wind","n":"Wind","type":"bool","def":true},
                {"k":"city","n":"City","type":"text","def":"","max":4}]}
        """#
        let info = try LinkInfo(Data(json.utf8))
        XCTAssertEqual(info.appID, "wifi-weather")
        XCTAssertEqual(info.channels, ["location", "time"])
        XCTAssertEqual(info.settings.map(\.key), ["units", "refresh", "wind", "city"])
        let units = info.settings[0], refresh = info.settings[1], wind = info.settings[2], city = info.settings[3]
        XCTAssertEqual(units.label, "温度单位")
        XCTAssertEqual(units.defaultValue, .text("C"))
        XCTAssertEqual(refresh.defaultValue, .int(30))
        XCTAssertEqual(wind.defaultValue, .bool(true))
        XCTAssertTrue(units.accepts(.text("F")))
        XCTAssertFalse(units.accepts(.text("K")))
        XCTAssertTrue(refresh.accepts(.int(5)))
        XCTAssertFalse(refresh.accepts(.int(181)))
        XCTAssertFalse(refresh.accepts(.text("30")))
        XCTAssertTrue(wind.accepts(.bool(false)))
        XCTAssertTrue(city.accepts(.text("abcd")))
        XCTAssertFalse(city.accepts(.text("北京")))      // 6 UTF-8 bytes > 4
    }

    func testMessages() throws {
        XCTAssertEqual(LinkIncoming(Data(#"{"t":"settings","v":{"units":"F","refresh":15,"wind":false}}"#.utf8)),
                       .settings(["units": .text("F"), "refresh": .int(15), "wind": .bool(false)]))
        XCTAssertEqual(LinkIncoming(Data(#"{"t":"err","why":"invalid value","k":"refresh"}"#.utf8)),
                       .error(why: "invalid value", key: "refresh"))
        XCTAssertEqual(LinkIncoming(Data(#"{"t":"req","ch":"location"}"#.utf8)), .request(channel: "location"))
        XCTAssertNil(LinkIncoming(Data(#"{"t":"nope"}"#.utf8)))
        XCTAssertNil(LinkIncoming(Data("not json".utf8)))

        XCTAssertEqual(String(decoding: LinkOutgoing.get.data, as: UTF8.self), #"{"t":"get"}"#)
        XCTAssertEqual(String(decoding: LinkOutgoing.set(["refresh": .int(15)]).data, as: UTF8.self),
                       #"{"t":"set","v":{"refresh":15}}"#)
        let time = LinkOutgoing.time(Date(timeIntervalSince1970: 1_763_640_000),
                                     timeZone: TimeZone(secondsFromGMT: 8 * 3600)!)
        XCTAssertEqual(String(decoding: time.data, as: UTF8.self),
                       #"{"ch":"time","d":{"tz":480,"unix":1763640000},"t":"push"}"#)
        let loc = LinkOutgoing.location(latitude: 39.987654321, longitude: 116.3, accuracy: 35.4,
                                        name: "Haidian, Beijing", nameZh: "北京市海淀区")
        let obj = try XCTUnwrap(JSONSerialization.jsonObject(with: loc.data) as? [String: Any])
        let d = try XCTUnwrap(obj["d"] as? [String: Any])
        XCTAssertEqual(d["lat"] as? Double, 39.98765)
        XCTAssertEqual(d["acc"] as? Int, 35)
        XCTAssertEqual(d["name"] as? String, "Haidian, Beijing")
        XCTAssertEqual(d["name_zh"] as? String, "北京市海淀区")
    }
}
