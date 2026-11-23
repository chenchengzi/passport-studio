import CoreLocation
import Foundation
import Observation

/// Current location with English and Chinese place names; devices pick the one
/// their fonts and language setting support. When-In-Use permission: updates
/// arrive while the app is in use.
@MainActor
@Observable
final class LocationProvider: NSObject {
    struct Fix: Equatable {
        var latitude: Double
        var longitude: Double
        var accuracy: Double
        var name: String
        var nameZh: String
    }

    private(set) var current: Fix?
    private(set) var authorization: CLAuthorizationStatus
    private(set) var lastError: String?
    /// Called after `current` changes by more than `minimumMove` meters.
    var onChange: (() -> Void)?

    private let manager = CLLocationManager()
    private var lastSent: CLLocation?
    private let minimumMove: CLLocationDistance = 500

    override init() {
        authorization = manager.authorizationStatus
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyHundredMeters
        manager.distanceFilter = 200
    }

    var denied: Bool { authorization == .denied || authorization == .restricted }

    /// Ask for permission if needed and start updating.
    func request() {
        switch manager.authorizationStatus {
        case .notDetermined: manager.requestWhenInUseAuthorization()
        case .authorizedWhenInUse, .authorizedAlways: manager.startUpdatingLocation()
        default: break
        }
    }

    private func update(with location: CLLocation) {
        if let lastSent, location.distance(from: lastSent) < minimumMove, current != nil { return }
        lastSent = location
        Task { @MainActor in
            let english = await placeName(location, locale: "en_US", separator: ", ", ascending: true)
            let chinese = await placeName(location, locale: "zh_CN", separator: "", ascending: false)
            let fallback = String(format: "%.2f, %.2f", location.coordinate.latitude, location.coordinate.longitude)
            current = Fix(latitude: location.coordinate.latitude, longitude: location.coordinate.longitude,
                          accuracy: location.horizontalAccuracy, name: english ?? fallback,
                          nameZh: chinese ?? english ?? fallback)
            onChange?()
        }
    }

    /// "Haidian, Beijing" in English; "北京市海淀区" in Chinese (largest first).
    private func placeName(_ location: CLLocation, locale: String, separator: String,
                           ascending: Bool) async -> String? {
        // CLGeocoder allows one request at a time per instance.
        let marks = try? await CLGeocoder().reverseGeocodeLocation(location, preferredLocale: Locale(identifier: locale))
        guard let mark = marks?.first else { return nil }
        var parts = [mark.subLocality, mark.locality ?? mark.administrativeArea]
            .compactMap { $0 }.filter { !$0.isEmpty }
        if !ascending { parts.reverse() }
        return parts.isEmpty ? nil : parts.joined(separator: separator)
    }
}

extension LocationProvider: CLLocationManagerDelegate {
    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        MainActor.assumeIsolated {
            authorization = manager.authorizationStatus
            if authorization == .authorizedWhenInUse || authorization == .authorizedAlways {
                manager.startUpdatingLocation()
            }
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        MainActor.assumeIsolated {
            if let location = locations.last, location.horizontalAccuracy >= 0 { update(with: location) }
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        MainActor.assumeIsolated { lastError = error.localizedDescription }
    }
}
