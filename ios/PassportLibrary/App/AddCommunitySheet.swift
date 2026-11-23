import PassportKit
import SwiftUI

struct AddCommunitySheet: View {
    let initialText: String
    @Environment(LibraryModel.self) private var library
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""
    @State private var play: CommunityPlay?
    @State private var looking = false
    @State private var downloading = false
    @State private var progress = 0.0
    @State private var message: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("https://ai-passport.folotoy.cn/plays/?play=…", text: $text, axis: .vertical)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .lineLimit(1...4)
                        .onSubmit { Task { await lookUp() } }
                    Button {
                        Task { await lookUp() }
                    } label: {
                        if looking { ProgressView() } else { Text("查找玩法") }
                    }
                    .disabled(text.trimmingCharacters(in: .whitespaces).isEmpty || looking || downloading)
                } header: {
                    Text("社区玩法链接")
                } footer: {
                    Text("在玩法库网页或小程序里复制分享链接，也可以直接输入玩法 ID。")
                }

                if let play {
                    Section("玩法") {
                        HStack(alignment: .top, spacing: 12) {
                            AsyncImage(url: play.thumbnail.flatMap {
                                URL(string: $0, relativeTo: URL(string: "https://\(CommunityAPI.host)"))
                            }) { image in
                                image.resizable().scaledToFill()
                            } placeholder: {
                                Color.secondary.opacity(0.15)
                            }
                            .frame(width: 64, height: 64)
                            .clipShape(RoundedRectangle(cornerRadius: 10))
                            VStack(alignment: .leading, spacing: 4) {
                                Text(play.title.preferred).font(.headline)
                                if let author = play.author { Text(author).font(.subheadline).foregroundStyle(.secondary) }
                                if let size = play.firmware?.size {
                                    Text(Format.bytes(size)).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                        if downloading {
                            ProgressView(value: progress) { Text("下载并校验中…") }
                        } else {
                            Button("下载并保存到固件库") { Task { await download(play) } }
                                .disabled(play.firmware?.available != true)
                        }
                    }
                }

                if let message {
                    Section { Text(message).foregroundStyle(.red) }
                }
            }
            .navigationTitle("添加社区玩法")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { dismiss() }.disabled(downloading)
                }
            }
            .onAppear {
                text = initialText
                if !initialText.isEmpty { Task { await lookUp() } }
            }
        }
    }

    private func lookUp() async {
        message = nil
        play = nil
        guard let reference = PlayReference.parse(text) else {
            message = "看不懂这个链接。请粘贴 ai-passport.folotoy.cn 的玩法分享链接。"
            return
        }
        looking = true
        defer { looking = false }
        do {
            play = try await library.lookUp(reference)
        } catch let error as CommunityError {
            message = error.message
        } catch {
            message = error.localizedDescription
        }
    }

    private func download(_ play: CommunityPlay) async {
        downloading = true
        message = nil
        defer { downloading = false }
        do {
            let item = try await library.download(play) { value in
                Task { @MainActor in progress = value }
            }
            if let reason = item.compatibilityError {
                message = "已保存，但这个玩法不能通过安装器安装：\(reason)"
            } else {
                dismiss()
            }
        } catch let error as CommunityError {
            message = error.message
        } catch let error as LibraryError {
            message = error.message
        } catch {
            message = error.localizedDescription
        }
    }
}
