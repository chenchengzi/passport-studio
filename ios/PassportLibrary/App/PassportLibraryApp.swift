import SwiftUI

@main
struct PassportLibraryApp: App {
    @State private var library = LibraryModel()
    @State private var device = DeviceManager()
    @State private var wifi = WifiStore()
    @State private var link = LinkManager()

    var body: some Scene {
        WindowGroup {
            LibraryView()
                .environment(library)
                .environment(device)
                .environment(wifi)
                .environment(link)
                .onOpenURL { url in
                    // .bin files shared from Files or AirDrop.
                    if url.isFileURL { library.importFile(at: url) }
                }
        }
    }
}
