#include "FileDialogWin32.h"

#ifdef _WIN32

#include <shobjidl.h>
#include <windows.h>

#include <stdexcept>
#include <iterator>

namespace vkr {

namespace {

class ComApartment {
  public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        uninitialize_ = SUCCEEDED(result);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            throw std::runtime_error(
                "Could not initialize COM for file dialog");
    }
    ~ComApartment() {
        if (uninitialize_)
            CoUninitialize();
    }

  private:
    bool uninitialize_ = false;
};

template <typename T> class ComOwner {
  public:
    ~ComOwner() {
        if (value_)
            value_->Release();
    }
    T **put() { return &value_; }
    T *get() const { return value_; }

  private:
    T *value_ = nullptr;
};

} // namespace

std::optional<std::filesystem::path>
openGltfFileDialog(void *ownerWindow) {
    ComApartment apartment;
    ComOwner<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(dialog.put()));
    if (FAILED(result))
        throw std::runtime_error("Could not create the scene file dialog");

    const COMDLG_FILTERSPEC filters[] = {
        {L"glTF Scenes (*.glb;*.gltf)", L"*.glb;*.gltf"},
        {L"glTF Binary (*.glb)", L"*.glb"},
        {L"glTF JSON (*.gltf)", L"*.gltf"},
    };
    dialog.get()->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog.get()->SetFileTypeIndex(1);
    dialog.get()->SetTitle(L"Import glTF Scene");
    FILEOPENDIALOGOPTIONS options = 0;
    dialog.get()->GetOptions(&options);
    dialog.get()->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
                             FOS_FORCEFILESYSTEM);

    result = dialog.get()->Show(static_cast<HWND>(ownerWindow));
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return std::nullopt;
    if (FAILED(result))
        throw std::runtime_error("Scene file dialog failed");

    ComOwner<IShellItem> item;
    result = dialog.get()->GetResult(item.put());
    if (FAILED(result))
        throw std::runtime_error("Could not read the selected scene path");
    PWSTR value = nullptr;
    result = item.get()->GetDisplayName(SIGDN_FILESYSPATH, &value);
    if (FAILED(result) || !value)
        throw std::runtime_error("Selected scene has no filesystem path");
    std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path;
}

} // namespace vkr

#else

namespace vkr {
std::optional<std::filesystem::path> openGltfFileDialog(void *) {
    return std::nullopt;
}
} // namespace vkr

#endif
