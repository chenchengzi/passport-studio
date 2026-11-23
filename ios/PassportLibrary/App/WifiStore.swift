import Foundation
import Observation
import PassportKit
import Security

/// Wi-Fi networks to provision, in priority order. Kept in the iOS Keychain
/// because they include passwords; the device only ever reports SSIDs back.
@MainActor
@Observable
final class WifiStore {
    private(set) var networks: [WifiNetwork] = []
    var lastError: String?

    private static let service = "passport-library.wifi"
    private static let account = "networks"

    init() {
        networks = Self.load()
    }

    var isFull: Bool { networks.count >= WifiNetwork.maxCount }

    /// Insert or replace (matched by the original SSID).
    func upsert(_ network: WifiNetwork, replacing original: String?) {
        if let original, let index = networks.firstIndex(where: { $0.ssid == original }) {
            networks[index] = network
        } else if let index = networks.firstIndex(where: { $0.ssid == network.ssid }) {
            networks[index] = network
        } else if !isFull {
            networks.append(network)
        }
        save()
    }

    func remove(at offsets: IndexSet) {
        networks.remove(atOffsets: offsets)
        save()
    }

    func move(from source: IndexSet, to destination: Int) {
        networks.move(fromOffsets: source, toOffset: destination)
        save()
    }

    private func save() {
        do {
            let data = try JSONEncoder().encode(networks)
            let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                        kSecAttrService as String: Self.service,
                                        kSecAttrAccount as String: Self.account]
            var status = SecItemUpdate(query as CFDictionary, [kSecValueData as String: data] as CFDictionary)
            if status == errSecItemNotFound {
                var add = query
                add[kSecValueData as String] = data
                add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
                status = SecItemAdd(add as CFDictionary, nil)
            }
            if status != errSecSuccess { lastError = "无法保存到钥匙串（\(status)）" }
        } catch {
            lastError = error.localizedDescription
        }
    }

    private static func load() -> [WifiNetwork] {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service,
                                    kSecAttrAccount as String: account,
                                    kSecReturnData as String: true,
                                    kSecMatchLimit as String: kSecMatchLimitOne]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data,
              let list = try? JSONDecoder().decode([WifiNetwork].self, from: data) else { return [] }
        return list
    }
}
