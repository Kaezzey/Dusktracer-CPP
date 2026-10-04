#include "core/project_dialog.h"
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>

namespace dusk_project {
std::filesystem::path command_line_scene(const char* fallback) {
    int count = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    const auto path =
        arguments && count > 1 ? std::filesystem::path(arguments[1]) : std::filesystem::u8path(fallback);
    if (arguments) {
        LocalFree(arguments);
    }
    return path;
}

std::optional<std::filesystem::path> choose_path(void* owner, dialog_kind kind,
                                                 const std::filesystem::path& initial) {
    struct apartment {
        HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ~apartment() {
            if (SUCCEEDED(status)) {
                CoUninitialize();
            }
        }
    } com;
    auto check = [](HRESULT hr) {
        if (FAILED(hr)) {
            throw std::runtime_error("Windows could not open the project file dialog.");
        }
    };
    check(com.status);
    const bool save = kind == dialog_kind::save_scene, folder = kind == dialog_kind::open_folder;
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    check(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&dialog)));
    FILEOPENDIALOGOPTIONS options;
    check(dialog->GetOptions(&options));
    options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
    options |= folder ? FOS_PICKFOLDERS
               : save ? (FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES)
                      : FOS_FILEMUSTEXIST;
    check(dialog->SetOptions(options));
    check(dialog->SetTitle(folder ? L"Open project folder"
                           : save ? L"Save scene in a project folder"
                                  : L"Open Dusk scene"));
    if (!folder) {
        const COMDLG_FILTERSPEC filter = {L"Dusk scene (*.dusk)", L"*.dusk"};
        check(dialog->SetFileTypes(1, &filter));
        check(dialog->SetDefaultExtension(L"dusk"));
        if (save) {
            check(dialog->SetFileName(initial.empty() ? L"Scene.dusk" : initial.filename().c_str()));
        }
    }
    if (!initial.empty()) {
        Microsoft::WRL::ComPtr<IShellItem> directory;
        const auto path = std::filesystem::is_directory(initial) ? initial : initial.parent_path();
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&directory)))) {
            dialog->SetFolder(directory.Get());
        }
    }
    const HRESULT result = dialog->Show(static_cast<HWND>(owner));
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::nullopt;
    }
    check(result);
    Microsoft::WRL::ComPtr<IShellItem> item;
    check(dialog->GetResult(&item));
    PWSTR name = nullptr;
    check(item->GetDisplayName(SIGDN_FILESYSPATH, &name));
    std::filesystem::path path(name);
    CoTaskMemFree(name);
    return path;
}
} // namespace dusk_project
#else
namespace dusk_project {
std::filesystem::path command_line_scene(const char* fallback) {
    return std::filesystem::u8path(fallback);
}
std::optional<std::filesystem::path> choose_path(void*, dialog_kind, const std::filesystem::path&) {
    throw std::runtime_error("Native project dialogs are currently available on Windows.");
}
} // namespace dusk_project
#endif
