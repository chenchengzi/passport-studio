// Sender side of the install stream: turns the image into DATA/SKIP packets,
// keeps at most `window` bytes unacknowledged, and rewinds when the device
// reports a different position. Pure state; the BLE layer drives it.
import Foundation

public struct TransferPlanner {
    public let end: UInt32
    public let maxPayload: Int
    public let window: Int

    private let image: [UInt8]           // bytes from 0x0
    private let skipRuns: [Range<UInt32>] // 4 KB aligned all-0xFF sectors
    public private(set) var sendPosition: UInt32
    public private(set) var ackedPosition: UInt32
    private var inFlight: [(end: UInt32, cost: Int)] = []

    public init(image: FirmwareImage, maxPayload: Int, window: Int = InstallerProtocol.windowBytes,
                startPosition: UInt32 = InstallerLayout.streamStart) {
        self.image = [UInt8](image.bytes.prefix(Int(image.dataEnd)))
        self.end = image.dataEnd
        self.maxPayload = max(16, maxPayload)
        self.window = window
        self.sendPosition = startPosition
        self.ackedPosition = startPosition
        self.skipRuns = TransferPlanner.findSkipRuns(self.image, from: InstallerLayout.streamStart)
    }

    public var bytesInFlight: Int { inFlight.reduce(0) { $0 + $1.cost } }
    public var allSent: Bool { sendPosition >= end }
    public var isComplete: Bool { ackedPosition >= end }
    /// Bytes that actually cross the radio (skips excluded).
    public var payloadBytes: Int { Int(end - InstallerLayout.streamStart) - skipRuns.reduce(0) { $0 + $1.count } }

    static func findSkipRuns(_ bytes: [UInt8], from start: UInt32) -> [Range<UInt32>] {
        let sector = InstallerLayout.sector
        var runs: [Range<UInt32>] = []
        var at = Int(start)
        while at + sector <= bytes.count {
            if bytes[at..<(at + sector)].allSatisfy({ $0 == 0xFF }) {
                let runStart = at
                while at + sector <= bytes.count && bytes[at..<(at + sector)].allSatisfy({ $0 == 0xFF }) {
                    at += sector
                }
                runs.append(UInt32(runStart)..<UInt32(at))
            } else {
                at += sector
            }
        }
        return runs
    }

    /// Next packet to write, or nil when everything is sent or the window is full.
    public mutating func nextPacket() -> Data? {
        guard sendPosition < end, bytesInFlight < window else { return nil }
        let p = sendPosition
        // Data packets stop at run boundaries, so p is never inside a run unless
        // the device rewound to a skip boundary within it.
        if let run = skipRuns.first(where: { $0.contains(p) }) {
            let to = min(run.upperBound, end)
            sendPosition = to
            inFlight.append((to, 16))
            return InstallerProtocol.skip(offset: p, to: to)
        }
        var limit = min(UInt32(maxPayload), end - p)
        if let next = skipRuns.first(where: { $0.lowerBound > p }) {
            limit = min(limit, next.lowerBound - p)
        }
        let payload = Data(image[Int(p)..<Int(p + limit)])
        sendPosition = p + limit
        inFlight.append((sendPosition, payload.count + InstallerProtocol.dataHeader))
        return InstallerProtocol.data(offset: p, payload: payload)
    }

    /// Device confirmed everything before `position`.
    public mutating func acknowledge(_ position: UInt32) {
        guard position >= ackedPosition else { return }
        ackedPosition = min(position, end)
        inFlight.removeAll { $0.end <= position }
        if sendPosition < ackedPosition { sendPosition = ackedPosition }
    }

    /// Device expects `position` next (resync, resume, or timeout recovery).
    public mutating func rewind(to position: UInt32) {
        let p = min(max(position, InstallerLayout.streamStart), end)
        ackedPosition = p
        sendPosition = p
        inFlight.removeAll()
    }
}
