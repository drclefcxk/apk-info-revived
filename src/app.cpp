#include "app.h"

#include <chrono>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shobjidl.h>

#include "android/apk_reader.h"
#include "alpine/alpine_reader.h"
#include "container/file_map.h"
#include "detect.h"
#include "i18n.h"
#include "util/text.h"

namespace apkinfo {
namespace {

std::string to_utf8(const wchar_t* text) {
    if (!text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), length);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

}  // namespace

InfoDocument load_package(const std::string& utf8_path, double* elapsed_ms) {
    const auto started = std::chrono::steady_clock::now();
    auto finish = [&](InfoDocument document) {
        if (elapsed_ms) {
            const auto done = std::chrono::steady_clock::now();
            *elapsed_ms = std::chrono::duration<double, std::milli>(done - started).count();
        }
        return document;
    };

    InfoDocument document;
    document.file_path = utf8_path;
    document.file_name = basename(utf8_path);

    std::string error;
    auto file = FileMap::open(utf8_path, &error);
    if (!file) {
        document.error = "cannot open " + basename(utf8_path) + ": " + error;
        return finish(std::move(document));
    }
    document.file_size = file->size();

    const Detection detection = detect(file->bytes());
    document.badge = container_kind_name(detection.kind);

    switch (detection.kind) {
        case ContainerKind::AndroidApk:
        case ContainerKind::AndroidSplitBundle: {
            AndroidOptions options;
            return finish(read_android_package(file->bytes(), detection, utf8_path, options));
        }
        case ContainerKind::AlpinePackage: {
            AlpineOptions options;
            return finish(read_alpine_package(file->bytes(), detection, utf8_path, options));
        }
        case ContainerKind::Unknown:
        default: {
            document.error = "unrecognised file: " + detection.reason;
            return finish(std::move(document));
        }
    }
}

std::string open_package_dialog() {
    std::string result;
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool owns_com = SUCCEEDED(initialised);

    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        }
        // Only the filter *names* are localised; the patterns are fixed.
        const std::wstring packages_label = to_wide(TR("Packages (Android or Alpine)"));
        const std::wstring android_label = to_wide(TR("Android packages"));
        const std::wstring all_label = to_wide(TR("All files"));
        const COMDLG_FILTERSPEC filters[] = {
            {packages_label.c_str(), L"*.apk;*.apks;*.apkm;*.xapk;*.zip"},
            {android_label.c_str(), L"*.apk;*.apks;*.apkm;*.xapk"},
            {all_label.c_str(), L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        const std::wstring dialog_title = to_wide(TR("Open a package"));
        dialog->SetTitle(dialog_title.c_str());        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = to_utf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (owns_com) CoUninitialize();
    return result;
}

std::string save_report_dialog(const std::string& suggested_name) {
    std::string result;
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool owns_com = SUCCEEDED(initialised);

    IFileSaveDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
        }
        const std::wstring text_label = to_wide(TR("Text report (*.txt)"));
        const std::wstring json_label = to_wide(TR("JSON report (*.json)"));
        const std::wstring all_label = to_wide(TR("All files"));
        const COMDLG_FILTERSPEC filters[] = {
            {text_label.c_str(), L"*.txt"},
            {json_label.c_str(), L"*.json"},
            {all_label.c_str(), L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        const std::wstring title = to_wide(TR("Save the package information"));
        dialog->SetTitle(title.c_str());
        dialog->SetFileName(to_wide(suggested_name).c_str());
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = to_utf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (owns_com) CoUninitialize();
    return result;
}

std::string save_icon_dialog(const std::string& suggested_name) {
    std::string result;
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool owns_com = SUCCEEDED(initialised);

    IFileSaveDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
        }
        const std::wstring png_label = to_wide(TR("PNG image (*.png)"));
        const std::wstring all_label = to_wide(TR("All files"));
        const COMDLG_FILTERSPEC filters[] = {
            {png_label.c_str(), L"*.png"},
            {all_label.c_str(), L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        const std::wstring title = to_wide(TR("Save the application icon"));
        dialog->SetTitle(title.c_str());
        dialog->SetFileName(to_wide(suggested_name).c_str());
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = to_utf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (owns_com) CoUninitialize();
    return result;
}

}  // namespace apkinfo
