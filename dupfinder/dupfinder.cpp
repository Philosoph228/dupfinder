#include <Windows.h>
#include "resource.h"
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <Shlwapi.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <openssl/sha.h> // Requires OpenSSL for SHA-256 hashing
#include <locale>
#include <iomanip>
#include <functional>
#include <format>
#include <thread>
#include <map>
#include <string>
#include <algorithm>
#include <shared_mutex>

namespace fs = std::filesystem;

std::wstring convert_to_wstring(const char* narrow_str) {
    if (!narrow_str) return L"";
    return std::wstring(narrow_str, narrow_str + std::strlen(narrow_str));
}

// Helper function to compute SHA-256 hash of a file
std::wstring compute_file_hash(const fs::path& file_path, std::function<void(std::wstring)> logCallback = [](std::wstring) {}) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path.string());
    }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    constexpr size_t buffer_size = 8192;
    char buffer[buffer_size];
    // size_t total_read = 0;
    while (file.read(buffer, buffer_size)) {
        SHA256_Update(&ctx, buffer, file.gcount());
        // total_read += file.gcount();
        // std::wcout << L"\rHashing: " << file_path.wstring() << L" (" << total_read << L" bytes processed)" << std::flush;
    }
    // Update for any remaining bytes
    SHA256_Update(&ctx, buffer, file.gcount());
    // total_read += file.gcount();
    // std::wcout << L"\rHashing: " << file_path.wstring() << L" (" << total_read << L" bytes processed)" << std::flush;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    std::wstringstream result;
    for (unsigned char c : hash) {
        result << std::setw(2) << std::setfill(L'0') << std::hex << static_cast<int>(c);
    }
    // std::wcout << L"\rHashing completed: " << file_path.wstring() << L"                    " << std::endl;
    logCallback(std::format(L"Hashing completed: {}\r\n", file_path.wstring()));
    return result.str();
}

// Function to find duplicate files by hash
std::unordered_map<std::wstring, std::vector<fs::path>> find_duplicate_files(const fs::path& root, std::function<void(std::wstring)> logCallback = [](std::wstring) {}) {
    std::unordered_map<std::wstring, std::vector<fs::path>> hash_to_files;

    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            try {
                auto hash = compute_file_hash(entry.path(), logCallback);
                hash_to_files[hash].push_back(entry.path());
            }
            catch (const std::exception& e) {
                std::wstring error_message = convert_to_wstring(e.what());
                logCallback(std::format(L"Error processing file {}: {}\r\n", entry.path().wstring(), error_message));
            }
        }
    }

    // Remove entries with only one file (unique files)
    for (auto it = hash_to_files.begin(); it != hash_to_files.end();) {
        if (it->second.size() < 2) {
            it = hash_to_files.erase(it);
        }
        else {
            ++it;
        }
    }

    return hash_to_files;
}

BOOL InitInstance(HINSTANCE hInstance) {
    return TRUE;
}

std::wstring SelectDirectory(HWND hwndOwner) {
    std::wstring folderPath;

    // Initialize COM
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        ::MessageBox(nullptr, L"Failed to initialize COM.", L"Error", MB_OK | MB_ICONERROR);
        return folderPath;
    }

    // Create the File Open Dialog object
    IFileDialog* pFileDialog = nullptr;
    hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileDialog));
    if (SUCCEEDED(hr)) {
        // Set options to select folders
        DWORD dwOptions;
        hr = pFileDialog->GetOptions(&dwOptions);
        if (SUCCEEDED(hr)) {
            hr = pFileDialog->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }

        // Show the dialog
        hr = pFileDialog->Show(hwndOwner);
        if (SUCCEEDED(hr)) {
            // Retrieve the selected folder
            IShellItem* pShellItem = nullptr;
            hr = pFileDialog->GetResult(&pShellItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszFolderPath = nullptr;
                hr = pShellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFolderPath);
                if (SUCCEEDED(hr)) {
                    // Store the folder path
                    folderPath = pszFolderPath;
                    CoTaskMemFree(pszFolderPath);
                }
                pShellItem->Release();
            }
        }

        pFileDialog->Release();
    }

    // Uninitialize COM
    CoUninitialize();

    return folderPath;
}

LRESULT CALLBACK ListViewCustomDraw(HWND hwnd, LPARAM lParam) {
    LPNMLVCUSTOMDRAW pCustomDraw = (LPNMLVCUSTOMDRAW)lParam;

    switch (pCustomDraw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        // Request item-specific notifications.
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
        // Request subitem-specific notifications.
        return CDRF_NOTIFYSUBITEMDRAW;

    case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
        // Customize label drawing for the subitem
        if (pCustomDraw->iSubItem == 0) { // Assuming column 0 is for labels
            RECT rect = pCustomDraw->nmcd.rc;
            std::wstring text(256, L'\0');
            ListView_GetItemText(hwnd, pCustomDraw->nmcd.dwItemSpec, 0, &text[0], 256);

            HDC hdc = pCustomDraw->nmcd.hdc;
            // HFONT hOldFont = (HFONT)SelectObject(hdc, pCustomDraw->nmcd.hFont);

            // Set text color
            COLORREF clrText = (pCustomDraw->nmcd.uItemState & CDIS_SELECTED)
                ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                : pCustomDraw->clrText;
            SetTextColor(hdc, clrText);

            // Draw the text with ellipsis if it doesn't fit
            DrawText(hdc, text.c_str(), -1, &rect, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE | DT_VCENTER);

            // Restore original font
            // SelectObject(hdc, hOldFont);

            // Indicate that we handled the drawing
            return CDRF_SKIPDEFAULT;
        }
        break;
    }
    }

    return CDRF_DODEFAULT;
}

// Helper function to retrieve a thumbnail for a file
HBITMAP GetThumbnail(const std::wstring& filePath, SIZE size) {
    HBITMAP hThumbnail = nullptr;

    // Create an IShellItem for the file
    IShellItem* pShellItem = nullptr;
    HRESULT hr = ::SHCreateItemFromParsingName(filePath.c_str(), nullptr, IID_PPV_ARGS(&pShellItem));
    if (SUCCEEDED(hr)) {
        // Get the IShellItemImageFactory interface
        IShellItemImageFactory* pImageFactory = nullptr;
        hr = pShellItem->QueryInterface(IID_PPV_ARGS(&pImageFactory));
        if (SUCCEEDED(hr)) {
            // Retrieve the thumbnail
            hr = pImageFactory->GetImage(size, SIIGBF_RESIZETOFIT, &hThumbnail);
            if (SUCCEEDED(hr)) {
                // Create a compatible DC for drawing
                HDC hScreenDC = GetDC(nullptr);
                HDC hMemDC = CreateCompatibleDC(hScreenDC);

                // Create a new bitmap
                HBITMAP hResultBitmap = CreateCompatibleBitmap(hScreenDC, size.cx, size.cy);
                SelectObject(hMemDC, hResultBitmap);

                // Create a DC for the thumbnail
                HDC hThumbnailDC = CreateCompatibleDC(hScreenDC);
                SelectObject(hThumbnailDC, hThumbnail);

                // Draw the thumbnail onto the background
                BITMAP bm;
                GetObject(hThumbnail, sizeof(BITMAP), &bm);

                // Calculate scaling and centering
                int thumbWidth = bm.bmWidth;
                int thumbHeight = bm.bmHeight;

                float aspectRatioThumb = (float)thumbWidth / thumbHeight;
                float aspectRatioTarget = (float)size.cx / size.cy;

                int drawWidth, drawHeight, offsetX, offsetY;

                if (aspectRatioThumb > aspectRatioTarget) {
                    // Landscape or square, fit by width
                    drawWidth = size.cx;
                    drawHeight = (int)(size.cx / aspectRatioThumb);
                    offsetX = 0;
                    offsetY = (size.cy - drawHeight) / 2;
                }
                else {
                    // Portrait, fit by height
                    drawHeight = size.cy;
                    drawWidth = (int)(size.cy * aspectRatioThumb);
                    offsetX = (size.cx - drawWidth) / 2;
                    offsetY = 0;
                }

                // Draw the thumbnail onto the background
                StretchBlt(hMemDC, offsetX, offsetY, drawWidth, drawHeight, hThumbnailDC, 0, 0, thumbWidth, thumbHeight, SRCCOPY);

                // Clean up
                DeleteDC(hThumbnailDC);
                DeleteDC(hMemDC);
                ReleaseDC(nullptr, hScreenDC);

                // Replace the thumbnail with the result bitmap
                DeleteObject(hThumbnail);
                hThumbnail = hResultBitmap;
            }
            pImageFactory->Release();
        }
        pShellItem->Release();
    }

    return hThumbnail;
}

#pragma pack(push, 1)
typedef struct tagDLGTEMPLATEEX__1 {
    WORD dlgVer;
    WORD signature;
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    WORD cDlgItems;
    short x;
    short y;
    short cx;
    short cy;
};
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct tagDLGTEMPLATEEX__2 {
    WORD pointsize;
    WORD weight;
    BYTE italic;
    BYTE charset;
};
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct tagDLGTEMPLATEEX {
    WORD dlgVer;
    WORD signature;
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    WORD cDlgItems;
    short x;
    short y;
    short cx;
    short cy;
    PWSTR menu;
    PWSTR windowClass;
    PWSTR title;
    WORD pointsize;
    WORD weight;
    BYTE italic;
    BYTE charset;
    PWSTR typeface;
} DLGTEMPLATEEX;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct tagDLGITEMTEMPLATEEX__1 {
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    short x;
    short y;
    short cx;
    short cy;
    DWORD id;
};
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct tagDLGITEMTEMPLATEEX {
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    short x;
    short y;
    short cx;
    short cy;
    DWORD id;
    PWSTR windowClass;
    PWSTR title;
    WORD extraCount;
} DLGITEMTEMPLATEEX;
#pragma pack(pop)

void* ReadDlgTemplateEx(void* buffer, DLGTEMPLATEEX* dlgTemplateEx) {
    void* head = buffer;
    memcpy(dlgTemplateEx, head, sizeof(struct tagDLGTEMPLATEEX__1));
    head = reinterpret_cast<unsigned char*>(head) + sizeof(struct tagDLGTEMPLATEEX__1);

    size_t len;

    // Read "menu"
    if (*reinterpret_cast<WORD*>(head) == 0xFFFF) {
        head = reinterpret_cast<WORD*>(head) + 1;
        dlgTemplateEx->menu = reinterpret_cast<PWSTR>(*reinterpret_cast<WORD*>(head));
        head = reinterpret_cast<WORD*>(head) + 1;
    }
    else {
        len = wcslen(reinterpret_cast<PWSTR>(head));
        dlgTemplateEx->menu = reinterpret_cast<PWSTR>(head);

        head = reinterpret_cast<WCHAR*>(head) + len + 1;
    }



    // Read "windowClass"
    len = wcslen(reinterpret_cast<PWSTR>(head));
    dlgTemplateEx->windowClass = reinterpret_cast<PWSTR>(head);

    head = reinterpret_cast<WCHAR*>(head) + len + 1;

    // Read "title"
    len = wcslen(reinterpret_cast<PWSTR>(head));
    dlgTemplateEx->title = reinterpret_cast<PWSTR>(head);

    head = reinterpret_cast<WCHAR*>(head) + len + 1;

    // Read next
    memcpy(dlgTemplateEx, head, sizeof(struct tagDLGTEMPLATEEX__2));
    head = reinterpret_cast<unsigned char*>(head) + sizeof(struct tagDLGTEMPLATEEX__2);

    // Read "typeface"
    len = wcslen(reinterpret_cast<PWSTR>(head));
    dlgTemplateEx->typeface = reinterpret_cast<PWSTR>(head);

    head = reinterpret_cast<WCHAR*>(head) + len + 1;

    // Align head to the next DWORD boundary
    head = reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(head) + 3) & ~static_cast<uintptr_t>(3)
        );

    return head;
}

void* ReadDlgItemTemplateEx(void* buffer, DLGITEMTEMPLATEEX* dlgItemTemplateEx) {
    void* head = buffer;
    memcpy(dlgItemTemplateEx, head, sizeof(struct tagDLGITEMTEMPLATEEX__1));
    head = reinterpret_cast<unsigned char*>(head) + sizeof(struct tagDLGITEMTEMPLATEEX__1);

    size_t len;

    // Read "windowClass"
    len = wcslen(reinterpret_cast<PWSTR>(head));
    dlgItemTemplateEx->windowClass = reinterpret_cast<PWSTR>(head);

    head = reinterpret_cast<WCHAR*>(head) + len + 1;

    // Read "title"
    len = wcslen(reinterpret_cast<PWSTR>(head));
    dlgItemTemplateEx->title = reinterpret_cast<PWSTR>(head);

    head = reinterpret_cast<WCHAR*>(head) + len + 1;

    // extraCount
    // dlgItemTemplateEx->extraCount = *reinterpret_cast<WORD*>(head);
    // head = reinterpret_cast<WORD*>(head) + 2;

    // Align head to the next DWORD boundary
    head = reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(head) + 3) & ~static_cast<uintptr_t>(3)
        );

    return head;
}

#define RECTWIDTH(x)((x)->right - (x)->left)
#define RECTHEIGHT(x)((x)->bottom - (x)->top)

void OpenFileLocation(std::wstring path) {
    if (PathFileExists(path.c_str())) {
        PIDLIST_ABSOLUTE pidl = ::ILCreateFromPath(path.c_str());
        if (pidl) {
            ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ::ILFree(pidl);
        }
    }
}

struct DirectoryEntry {
    HANDLE m_handle;
    std::vector<std::wstring> m_files;
};

constexpr int BUFFER_SIZE = 1024;

struct Overlapped : OVERLAPPED {
    fs::path directory;
    DirectoryEntry* dirEntry;
    BYTE buffer[BUFFER_SIZE];
};

class FileWatcher {
public:
    FileWatcher() {
        m_hCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (!m_hCompletionPort) {
            throw std::runtime_error("Failed to create IO completion port.");
        }
    }

    void AddFile(fs::path path) {
        std::unique_lock<std::shared_mutex> lock(m_mapMutex);

        if (fs::is_regular_file(path)) {
            auto dir = path.parent_path();
            auto filename = path.filename();

            auto it = m_directories.find(dir);
            if (it != m_directories.end()) {
                m_directories[dir].m_files.push_back(filename.wstring());
            }
            else {
                HANDLE hDirectory = ::CreateFile(
                    dir.wstring().c_str(),
                    FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                    nullptr);
                if (!hDirectory || hDirectory == INVALID_HANDLE_VALUE) {
                    throw std::runtime_error("Failed to get directory handle.");
                }

                DirectoryEntry dirEntry;
                dirEntry.m_handle = hDirectory;

                m_hCompletionPort = ::CreateIoCompletionPort(hDirectory, m_hCompletionPort, 0, 1);
                if (!m_hCompletionPort) {
                    throw std::runtime_error("Failed to create IO completion port.");
                }

                m_directories[dir] = dirEntry;
                m_directories[dir].m_files.push_back(filename.wstring());

                // Create custom OVERLAPPED structure
                Overlapped* pCustomOverlapped = new Overlapped();
                memset(pCustomOverlapped, 0, sizeof(Overlapped));
                pCustomOverlapped->directory = dir;
                pCustomOverlapped->dirEntry = &m_directories[dir];

                // pCustomOverlapped->hEvent = hDirectory;

                // If the function succeeds, the return value is nonzero
                if (!::ReadDirectoryChangesW(
                    hDirectory,
                    pCustomOverlapped->buffer,
                    BUFFER_SIZE,
                    FALSE,  // Don't monitor subdirectories
                    FILE_NOTIFY_CHANGE_FILE_NAME,   // Watch for file name changes
                    nullptr,
                    static_cast<LPOVERLAPPED>(pCustomOverlapped),
                    nullptr))
                {
                    ::CloseHandle(dirEntry.m_handle);
                    m_directories.erase(dir);
                    delete pCustomOverlapped;
                    throw std::runtime_error(std::format("Failed to read directory changes. Error code: {}", ::GetLastError()));
                }
            }
        }
        else {
            throw std::runtime_error("FileWatcher: Added path is not a file");
        }
    }

    void SetCallback(std::function<void(fs::path)> callback) {
        m_callback = callback;
    }

    void operator()() {
        WatcherThread();
    }

    HANDLE m_hCompletionPort;

    void WatcherThread() {
        DWORD dwBytesTransferred;
        ULONG_PTR completionKey;
        OVERLAPPED* pOverlapped;

        while (true) {
            if (!::GetQueuedCompletionStatus(m_hCompletionPort, &dwBytesTransferred, &completionKey, &pOverlapped, INFINITE)) {
                throw std::runtime_error("Failed to get completion status.");
            }

            if (pOverlapped) {
                Overlapped* pCustomOverlapped = static_cast<Overlapped*>(pOverlapped);
                BYTE* buffer = reinterpret_cast<BYTE*>(pCustomOverlapped->buffer);

                if (dwBytesTransferred > 0) {
                    /*
                    Залочить мьютекс, проверить что dirEntry существует
                    */
                    std::shared_lock<std::shared_mutex> lock(m_mapMutex);

                    FILE_NOTIFY_INFORMATION* pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);

                    do {
                        std::wstring changedFile(pNotify->FileName, pNotify->FileNameLength / sizeof(WCHAR));

                        switch (pNotify->Action) {
                        case FILE_ACTION_REMOVED: {
                            // Using std::find to check if changed file name exists in dirEntry
                            auto& dirEntry = pCustomOverlapped->dirEntry;
                            auto& files = dirEntry->m_files;

                            auto it = std::find(files.begin(), files.end(), changedFile);
                            if (it != files.end()) {
                                fs::path fullPath = pCustomOverlapped->directory / *it;
                                if (::GetFileAttributes(fullPath.wstring().c_str()) == INVALID_FILE_ATTRIBUTES && ::GetLastError() == ERROR_FILE_NOT_FOUND) {
                                    m_callback(fullPath);

                                    // Remove deleted file from list
                                    files.erase(it);

                                    // Close directory handle if there no files left
                                    if (files.empty()) {
                                        ::CloseHandle(dirEntry->m_handle);
                                        m_directories.erase(pCustomOverlapped->directory);
                                        continue;
                                    }
                                }
                            }
                        }
                                                break;

                        case FILE_ACTION_RENAMED_OLD_NAME: {
                            auto message = std::format(L"File renamed. Old name: {}", changedFile);
                            ::MessageBox(nullptr, message.c_str(), L"Information", MB_OK | MB_ICONERROR);
                        }
                                                         break;

                        case FILE_ACTION_RENAMED_NEW_NAME: {
                            auto message = std::format(L"File renamed. New name: {}", changedFile);
                            ::MessageBox(nullptr, message.c_str(), L"Information", MB_OK | MB_ICONERROR);
                        }
                                                         break;
                        }

                        pNotify = pNotify->NextEntryOffset
                            ? reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                                reinterpret_cast<BYTE*>(pNotify) + pNotify->NextEntryOffset)
                            : nullptr;
                    } while (pNotify);

                    if (!::ReadDirectoryChangesW(
                        pCustomOverlapped->dirEntry->m_handle,
                        pCustomOverlapped->buffer,
                        BUFFER_SIZE,
                        FALSE,  // Don't monitor subdirectories
                        FILE_NOTIFY_CHANGE_FILE_NAME,   // Watch for file name changes
                        nullptr,
                        static_cast<LPOVERLAPPED>(pCustomOverlapped),
                        nullptr))
                    {
                        ::CloseHandle(pCustomOverlapped->dirEntry->m_handle);
                        m_directories.erase(pCustomOverlapped->directory);
                        delete pCustomOverlapped;
                        throw std::runtime_error(std::format("Failed to read directory changes. Error code: {}", ::GetLastError()));
                    }
                }
                else {
                    delete pCustomOverlapped;
                }
            }
        }
    }

private:
    std::shared_mutex m_mapMutex;
    std::map<fs::path, DirectoryEntry> m_directories;
    std::function<void(fs::path)> m_callback;
};

FileWatcher g_fileWatcher;

class DynamicDLL {
public:
    // Delete copy cosntructor and assigment operator
    DynamicDLL(const DynamicDLL&) = delete;
    DynamicDLL& operator=(const DynamicDLL&) = delete;

    // Default move constructor and assigment operator
    DynamicDLL(DynamicDLL&&) noexcept = default;
    DynamicDLL& operator=(DynamicDLL&&) noexcept = default;

    explicit DynamicDLL(std::wstring name, bool load = false) : m_name(name), m_hModule(nullptr) {
        if (load) {
            Load();
        }
    }

    virtual ~DynamicDLL() {
        Release();
    }

    void Load() {
        if (m_hModule) {
            throw std::logic_error("Library already loaded");
        }

        HMODULE hModule = ::LoadLibrary(m_name.c_str());
        if (!hModule) {
            throw std::runtime_error("Failed to load library");
        }

        m_hModule = hModule;
    }

    void Release() {
        if (m_hModule) {
            ::FreeLibrary(m_hModule);
            m_hModule = nullptr;
        }
    }

    template<typename T, typename... Args>
    auto CallOnce(std::string name, Args&&... args) -> typename std::invoke_result<T, Args...>::type {
        auto fn = LoadFunction<T>(name);
        return fn(std::forward<Args>(args)...);
    }

    template<typename T>
    T LoadFunction(std::string name) const {
        if (!m_hModule) {
            throw std::logic_error("Library not loaded");
        }

        FARPROC proc = ::GetProcAddress(m_hModule, name.c_str());
        if (!proc) {
            throw std::runtime_error("Function not found");
        }

        return reinterpret_cast<T>(proc);
    }

    [[nodiscard]] bool IsLoaded() const noexcept {
        return m_hModule != nullptr;
    }

private:
    HMODULE m_hModule;
    std::wstring m_name;
};

class Window;

class WindowMap {
public:
    Window* GetWindow(HWND hwnd) const;

    void Add(HWND hwnd, Window* window);
    void Clear();
    void Remove(HWND hwnd);
    void Remove(Window* window);

    static WindowMap& Instance();

private:
    std::map<HWND, Window*> m_windowMap;
};

class DynamicLayoutItem;
class DynamicLayoutData;

class DynamicLayout {
public:
    struct MoveSettings {
        MoveSettings() : m_nXRatio(0), m_nYRatio(0) {}

        bool IsHorizontal() const { return m_nXRatio > 0; }
        bool IsVertical() const { return m_nYRatio > 0; }
        bool IsNone() const { return !IsHorizontal() && !IsVertical(); };

        int m_nXRatio;
        int m_nYRatio;
    };

    struct SizeSettings {
        SizeSettings() : m_nXRatio(0), m_nYRatio(0) {}

        bool IsHorizontal() const { return m_nXRatio > 0; }
        bool IsVertical() const { return m_nYRatio > 0; }
        bool IsNone() const { return !IsHorizontal() && !IsVertical(); };

        int m_nXRatio;
        int m_nYRatio;
    };

public:
    DynamicLayout() : m_pHostWnd(nullptr) {}
    virtual ~DynamicLayout() = default;

    BOOL Create(Window* pHostWnd);
    UINT AdjustItemRect(const DynamicLayoutItem& item, RECT& rc);
    void Adjust();
    BOOL AddItem(HWND hWnd, MoveSettings moveSettings, SizeSettings sizeSettings);
    DynamicLayoutItem* FindItem(HWND hWnd);
    BOOL PrepareItem(DynamicLayoutItem& item) const;
    RECT GetItemRect(DynamicLayoutItem& item) const;

    // This method the dynamic layout from AFX_DIALOG_LAYOUT resource and then applies the layout to the host window.
    static BOOL LoadResource(Window* pHostWnd, PVOID pResource, DWORD dwSize);

private:
    std::vector<DynamicLayoutItem*> m_listWnd;
    HWND m_hwnd;
    Window* m_pHostWnd;
};

struct DynamicLayoutItem {
public:
    struct Point {
        Point() : x(0.0), y(0.0) {}
        double x;
        double y;
    };

    DynamicLayoutItem(
        HWND hWnd,
        const DynamicLayout::MoveSettings& moveSettings,
        const DynamicLayout::SizeSettings& sizeSettings
    ) : hwnd(hWnd),
        m_moveSettings(moveSettings),
        m_sizeSettings(sizeSettings) {
    };

    HWND hwnd;
    Point m_ptInit;
    Point m_szInit;
    DynamicLayout::MoveSettings m_moveSettings;
    DynamicLayout::SizeSettings m_sizeSettings;
    double x;
};

class DynamicLayoutData {
public:
    struct Item {
        DynamicLayout::MoveSettings m_moveSettings;
        DynamicLayout::SizeSettings m_sizeSettings;
    };

    void CleanUp();
    BOOL ReadResource(PVOID pResource, UINT nSize);
    BOOL ApplyLayoutDataTo(Window* pHostWnd, BOOL bUpdate);

protected:
    std::vector<Item> m_listCtrls;
};

/******************
 * Implementation *
 ******************/ 

class Window {
public:
    virtual ~Window();

    void Attach(HWND hwnd);
    HWND Detach();
    void Destroy();
    BOOL IsWindow() const;
    LRESULT SendMessage(UINT message, WPARAM wParam, LPARAM lParam) const;
    BOOL Show(int cmd_show = SW_SHOWNORMAL) const;

    HWND GetHWND() const;
    void SetHWND(HWND hwnd);

    void ScreenToClient(PRECT pRect) const {
        if (!(m_hwnd && ::IsWindow(m_hwnd))) {
            throw;
        }
        ::ScreenToClient(m_hwnd, (PPOINT)pRect);
        ::ScreenToClient(m_hwnd, ((PPOINT)pRect) + 1);
    }

    BOOL SetPosition(HWND hwndInsertAfter, int x, int y, int w, int h, UINT flags) const;

    DWORD GetStyle() const;
    DWORD SetStyle(DWORD dwStyle) const;
    BOOL ModifyStyle(DWORD dwRemove, DWORD dwAdd, UINT nFlags = 0) const;

    DWORD GetExStyle() const;

    std::wstring GetText() const;
    LRESULT SetText(PCWSTR text) const;
    LRESULT SetText(const std::wstring& text) const;

    int GetTextLength() const;

    // Dynamic layout
    DynamicLayout* GetDynamicLayout();
    void EnableDynamicLayout(bool enable = true);
    void ResizeDynamicLayout();
    BOOL LoadDynamicLayoutResource(PCWSTR pszResourceName);
    BOOL InitDynamicLayout();
    bool IsDynamicLayoutEnabled() const;

    LRESULT DefaultWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    virtual LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

protected:
    virtual BOOL OnCommand(WPARAM wParam, LPARAM lParam);
    virtual void OnCreate(HWND hwnd, LPCREATESTRUCT lpcs);
    virtual BOOL OnDestroy();
    virtual void OnSize(UINT message, UINT type, SIZE size);
    HFONT m_hFont;
    HWND m_hwnd;
    WNDPROC m_pfnPrevWindowProc;

private:
    void Subclass(HWND hwnd);
    void UnSubclass();
    DynamicLayout* m_pDynamicLayout;
    static Window* m_currentWindow;
};

class Dialog : public Window {
public:
    virtual ~Dialog();
    INT_PTR Create(UINT resource_id, HWND parent, bool modal);
    virtual void EndDialog(INT_PTR result);
    HWND GetDlgItem(int id_item) const;

protected:
    virtual void OnCancel();
    virtual BOOL OnClose();
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    INT_PTR DlgProcDefault(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
    virtual INT_PTR DlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

private:
    static INT_PTR CALLBACK StaticDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
    bool m_modal;
    int m_resourceId;

protected:
    using Window::m_hwnd;
};

Window* WindowMap::GetWindow(HWND hwnd) const {
    auto it = m_windowMap.find(hwnd);
    return it != m_windowMap.end() ? it->second : nullptr;
}

void WindowMap::Add(HWND hwnd, Window* window) {
    if (hwnd && !GetWindow(hwnd)) {
        m_windowMap.insert(std::make_pair(hwnd, window));
    }
}

void WindowMap::Clear() {
    for (const auto& pair : m_windowMap) {
        const HWND hwnd = pair.first;
        if (::IsWindow(hwnd)) {
            ::DestroyWindow(hwnd);
        }
    }
    m_windowMap.clear();
}

void WindowMap::Remove(HWND hwnd) {
    m_windowMap.erase(hwnd);
}

void WindowMap::Remove(Window* window) {
    for (auto it = m_windowMap.begin(); it != m_windowMap.end(); ++it) {
        if (window == it->second) {
            m_windowMap.erase(it);
            return;
        }
    }
}

WindowMap& WindowMap::Instance() {
    static WindowMap instance;
    return instance;
}

class ImageList {
public:
    ImageList() : m_hImageList(nullptr) {};

    ~ImageList() {
        Destroy();
    };

    void operator=(const HIMAGELIST image_list) {
        SetHandle(image_list);
    }

    int AddBitmap(HBITMAP bitmap, COLORREF mask) const {
        if (mask != CLR_NONE) {
            return ::ImageList_AddMasked(m_hImageList, bitmap, mask);
        }
        else {
            return ::ImageList_Add(m_hImageList, bitmap, nullptr);
        }
    }

    bool Create(int cx, int cy) {
        Destroy();
        m_hImageList = ::ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 0, 0);
        return !!m_hImageList;
    }

    void Destroy() {
        if (m_hImageList) {
            ::ImageList_Destroy(m_hImageList);
            m_hImageList = nullptr;
        }
    }

    BOOL Remove(int index) const {
        return ::ImageList_Remove(m_hImageList, index);
    }

    HIMAGELIST GetHandle() const {
        return m_hImageList;
    }

    void SetHandle(HIMAGELIST image_list) {
        Destroy();

        m_hImageList = image_list;
    }

private:
    HIMAGELIST m_hImageList;
};

class ListView : public Window {
public:
    int InsertColumn(int index, int width, int width_min, int align, PCWSTR text) {
        if (!(m_hwnd && ::IsWindow(m_hwnd))) {
            throw std::runtime_error("Invalid window handle");
        }

        // If index is -1 and below, get column count and insert column as last column at the end
        if (index < 0) {
            HWND hHeader = reinterpret_cast<HWND>(::SendMessage(m_hwnd, LVM_GETHEADER, 0, 0));
            if (!hHeader) {
                throw std::runtime_error("Unable to get ListView header window handle. Can't count columns.");
                return 0;
            }
            index = ::SendMessage(hHeader, HDM_GETITEMCOUNT, 0, 0);
        }

        LVCOLUMN lvc{};
        lvc.cx = width;
        lvc.cxMin = width_min;
        lvc.fmt = align;
        lvc.mask = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH | (width_min ? LVCF_MINWIDTH : 0);
        lvc.pszText = const_cast<PWSTR>(text);
        return ListView_InsertColumn(m_hwnd, index, &lvc);
    }

    int InsertGroup(int index, PCWSTR text, bool collapsable, bool collapsed) {
        LVGROUP lvg{};
        lvg.cbSize = sizeof(lvg);
        lvg.iGroupId = index;
        lvg.mask = LVGF_HEADER | LVGF_GROUPID;
        lvg.pszHeader = const_cast<PWSTR>(text);

        if (collapsable) {
            lvg.mask |= LVGF_STATE;
            lvg.state - LVGS_COLLAPSIBLE;
            if (collapsed) {
                lvg.state |= LVGS_COLLAPSED;
            }
        }

        return ListView_InsertGroup(m_hwnd, index, &lvg);
    }

    int InsertItem(const LVITEM& lvi) {
        return ListView_InsertItem(m_hwnd, &lvi);
    }

    int InsertItem(int item, int group, int image, UINT column_count,
        PUINT columns, LPCWSTR text, LPARAM lParam) {
        LVITEM lvi = { 0 };
        lvi.cColumns = column_count;
        lvi.iGroupId = group;
        lvi.iImage = image;
        lvi.iItem = item;
        lvi.lParam = lParam;
        lvi.puColumns = columns;
        lvi.pszText = const_cast<LPWSTR>(text);

        if (column_count != 0)
            lvi.mask |= LVIF_COLUMNS;
        if (group > -1)
            lvi.mask |= LVIF_GROUPID;
        if (image > -1)
            lvi.mask |= LVIF_IMAGE;
        if (lParam != 0)
            lvi.mask |= LVIF_PARAM;
        if (text != nullptr)
            lvi.mask |= LVIF_TEXT;

        return ListView_InsertItem(m_hwnd, &lvi);
    }

    void SetImageList(HIMAGELIST image_list, int type) {
        ListView_SetImageList(m_hwnd, image_list, type);
    }

    void GetItemText(int item, int subitem, LPWSTR output, int max_length) {
        ListView_GetItemText(m_hwnd, item, subitem, output, max_length);
    }

    int SetView(DWORD view) {
        return ListView_SetView(m_hwnd, view);
    }

    void SetExplorerTheme() {
        if (!(m_hwnd && ::IsWindow(m_hwnd))) {
            throw std::runtime_error("Invalid window handle");
        }

        auto uxtheme = DynamicDLL(L"uxtheme.dll", true);
        uxtheme.CallOnce<HRESULT(__stdcall*)(HWND, PCWSTR, PCWSTR)>("SetWindowTheme", m_hwnd, L"Explorer", nullptr);
    }

    void SetIconSpacing(int horizontal, int vertical) {
        if (!(m_hwnd && ::IsWindow(m_hwnd))) {
            throw std::runtime_error("Invalid window handle");
        }

        ListView_SetIconSpacing(m_hwnd, horizontal, vertical);
    }

    void EnableGroupView(bool flag) const {
        if (!(m_hwnd && ::IsWindow(m_hwnd))) {
            throw std::runtime_error("Invalid window handle");
        }

        ::SendMessage(m_hwnd, LVM_ENABLEGROUPVIEW, static_cast<BOOL>(flag), 0);
    }
};

class Edit : public Window {
public:
    void AppendText(std::wstring text) {
        int len = GetTextLength();
        ::SendMessage(m_hwnd, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        ::SendMessage(m_hwnd, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    }
};

class DuplicateFilesListView : public ListView {
    int m_nextGroupId = 0;
    int m_nextItemId = 0;

    ImageList m_imageList;
public:
    void Attach(HWND hwnd) {
        ListView::Attach(hwnd);
        InitListView();
    }

    int InsertDuplicateGroup(std::wstring hash) {
        int insertedIndex = InsertGroup(m_nextGroupId, hash.c_str(), false, false);
        if (insertedIndex >= 0)
            m_nextGroupId = insertedIndex + 1;
        return insertedIndex;
    }

    int InsertDuplicateFileItem(std::wstring path, int iGroupId) {
        SIZE size = { 96, 96 };
        HBITMAP hBitmap = GetThumbnail(path, size);

        int insertedItem = 0;
        if (hBitmap) {
            // Add the thumbnail to the ImageList
            int imageIndex = m_imageList.AddBitmap(hBitmap, CLR_NONE);
            ::DeleteObject(hBitmap);
            insertedItem = InsertItem(m_nextItemId, iGroupId, imageIndex, -1, nullptr, path.c_str(), 0);
            if (insertedItem >= 0) {
                m_nextItemId = insertedItem + 1;
            }
        }

        return insertedItem;
    }

    void OpenShellMenuForItem(int index, POINT pt) {
        std::wstring filePath(MAX_PATH, L'\0');
        GetItemText(index, 0, &filePath[0], MAX_PATH);

        IShellItem* pShellItem = nullptr;

        // Create a shell item for the file
        HRESULT hr = ::SHCreateItemFromParsingName(filePath.c_str(), nullptr, IID_PPV_ARGS(&pShellItem));
        if (SUCCEEDED(hr)) {
            IContextMenu* pContextMenu = nullptr;

            // Get the IContextMenu interface
            hr = pShellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&pContextMenu));
            if (SUCCEEDED(hr)) {
                HMENU hMenu = ::CreatePopupMenu();
                if (hMenu) {
                    // Query for the default menu options
                    hr = pContextMenu->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_NORMAL);
                    if (SUCCEEDED(hr)) {

                        // Add a separator and custom menu items

                        InsertMenu(hMenu, 0, MF_BYPOSITION | MF_STRING, 0x8000, L"Open File location…");
                        InsertMenu(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

                        // Display the menu
                        int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);

                        if (cmd > 0) {
                            if (cmd >= 0x8000 && cmd <= 0x8001) {
                                // Handle custom actions
                                if (cmd == 0x8000) {
                                    OpenFileLocation(filePath);
                                }
                            }
                            else {
                                // Execute the shell command
                                // Execute the selected command
                                CMINVOKECOMMANDINFOEX cmi{};
                                cmi.cbSize = sizeof(cmi);
                                cmi.fMask = CMIC_MASK_UNICODE;
                                cmi.hwnd = m_hwnd;
                                cmi.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                                cmi.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                                cmi.nShow = SW_NORMAL;

                                hr = pContextMenu->InvokeCommand((LPCMINVOKECOMMANDINFO)&cmi);

                                /*
                                // Check if the file was deleted
                                if (!fs::exists(filePath)) {
                                    // Find the item in the ListView
                                    LVFINDINFO findInfo{};
                                    findInfo.flags = LVFI_STRING;
                                    findInfo.psz = filePath.c_str();

                                    int index = ListView_FindItem(g_hListResults, -1, &findInfo);
                                    if (index != -1) {
                                        ListView_DeleteItem(g_hListResults, index);
                                    }
                                }
                                */
                            }
                        }
                    }
                    ::DestroyMenu(hMenu);
                }
                pContextMenu->Release();
            }
            pShellItem->Release();
        }
    }

    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) override final {

        switch (message) {

        }

        return DefaultWndProc(hwnd, message, wParam, lParam);
    }

private:
    void InitListView() {
        // Enable Explorer-style theme
        SetExplorerTheme();

        // Enable grouping in the ListView
        ListView_SetExtendedListViewStyle(m_hwnd, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        EnableGroupView(true);

        m_imageList.Create(96, 96);
        SetImageList(m_imageList.GetHandle(), LVSIL_NORMAL);


        SetIconSpacing(128, 128);
        InsertColumn(-1, 640, 0, LVCFMT_LEFT, L"Name");
    }
};

class MainDlg : public Dialog {
private:

    BOOL OnInitDialog() override final {
        Dialog::OnInitDialog();

        HICON hIcon = ::LoadIcon(::GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON1));
        SendMessage(WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        SendMessage(WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));

        m_editPath.Attach(GetDlgItem(IDC_EDIT1));
        m_editLog.Attach(GetDlgItem(IDC_EDIT2));
        m_listView.Attach(GetDlgItem(IDC_LIST1));

        g_fileWatcher.SetCallback([=](fs::path path) -> void {
            // Find the item in the ListView
            LVFINDINFO findInfo{};
            findInfo.flags = LVFI_STRING;

            std::wstring wpath = path.wstring();
            findInfo.psz = wpath.c_str();

            int index = ListView_FindItem(m_listView.GetHWND(), -1, &findInfo);
            if (index != -1) {
                ListView_DeleteItem(m_listView.GetHWND(), index);
            }
            });

        return TRUE;
    }

    void EndDialog(INT_PTR result) override final {
        Dialog::EndDialog(result);
        ::PostQuitMessage(result);
    }

    BOOL OnDestroy() override final {
        ::PostQuitMessage(0);
        return TRUE;
    }

    BOOL OnCommand(WPARAM wParam, LPARAM lParam) override final {
        switch (LOWORD(wParam)) {
        case IDC_BUTTON1:
            if (HIWORD(wParam) == BN_CLICKED) {

                auto selectedFolder = ::SelectDirectory(m_hwnd);
                if (!selectedFolder.empty()) {
                    m_editPath.SetText(selectedFolder);
                }
                return TRUE;
                break;
            }
            break;

        case IDC_BUTTON2:
            if (HIWORD(wParam) == BN_CLICKED) {
                std::wstring selectedFolder = m_editPath.GetText();
                auto duplicates = find_duplicate_files(selectedFolder, [&](std::wstring message) {
                    m_editLog.AppendText(message);
                    });

                for (const auto& [hash, files] : duplicates) {
                    int groupId = m_listView.InsertDuplicateGroup(hash);
                    for (const auto& file : files) {
                        g_fileWatcher.AddFile(file);
                        m_listView.InsertDuplicateFileItem(file, groupId);
                    }
                }

                return TRUE;
                break;
            }
            break;

        case ID_VIEW_ICONS:
        case ID_VIEW_LIST:
        case ID_VIEW_DETAILS:
            m_listView.SetView(
                LOWORD(wParam) == ID_VIEW_ICONS ? LV_VIEW_ICON :
                LOWORD(wParam) == ID_VIEW_LIST ? LV_VIEW_LIST :
                LV_VIEW_DETAILS);
            return TRUE;
            break;
        }

        return FALSE;
    }

    INT_PTR DlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) override final {
        switch (message) {

        case WM_NOTIFY:
        {
            LPNMHDR pnmh = reinterpret_cast<LPNMHDR>(lParam);
            if (pnmh->idFrom == IDC_LIST1) {
                if (pnmh->code == NM_RCLICK) {
                    // Get the mouse position for the context menu
                    POINT pt;
                    ::GetCursorPos(&pt);

                    // Translate cursor position to ListtView client coordinates

                    POINT localPt;
                    localPt = pt;
                    ::ScreenToClient(pnmh->hwndFrom, &localPt);

                    // Identify the clicked item
                    LVHITTESTINFO hitTestInfo{};
                    hitTestInfo.pt = localPt;
                    int index = ListView_HitTest(pnmh->hwndFrom, &hitTestInfo);

                    if (index != -1 && (hitTestInfo.flags & LVHT_ONITEM)) {
                        m_listView.OpenShellMenuForItem(index, pt);
                    }
                    else {
                        // Show the context menu
                        HMENU hMenu = ::LoadMenu(::GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_MENU1));
                        HMENU hSubMenu = ::GetSubMenu(hMenu, 0);
                        ::TrackPopupMenu(hSubMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hDlg, nullptr);
                        ::DestroyMenu(hMenu);
                        return TRUE;
                    }


                }
                else if (pnmh->code == NM_CUSTOMDRAW) {
                    return ListViewCustomDraw(pnmh->hwndFrom, lParam);
                }
            }
        }
        return FALSE;
        break;
        }

        return DlgProcDefault(hDlg, message, wParam, lParam);
    }

    DuplicateFilesListView m_listView;
    Edit m_editPath;
    Edit m_editLog;
};

std::wstring CharToWChar(const std::string& str) {
    // Get the required size of the wide-character buffer
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    if (size_needed == 0) {
        throw std::runtime_error("Error in MultiByteToWideChar");
    }

    // Allocate the wide-character buffer
    std::wstring wstr(size_needed, 0);

    // Perform the conversion
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size_needed);

    return std::move(wstr);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {

    // std::locale::global(std::locale("en_US.UTF-8"));

    INITCOMMONCONTROLSEX iccex{};
    iccex.dwSize = sizeof(iccex);
    iccex.dwICC = ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&iccex);

    MSG msg{};

    try {
        std::thread fileWatcherThread(std::ref(g_fileWatcher));
        fileWatcherThread.detach();

        MainDlg dlg;
        dlg.Create(IDD_DIALOG1, nullptr, false);
        dlg.Show();

        while (::GetMessage(&msg, nullptr, 0, 0)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
    }
    catch (std::exception& e) {
        std::string message = e.what();
        ::MessageBoxA(nullptr, message.c_str(), nullptr, MB_OK | MB_ICONERROR);
    }

    return static_cast<int>(msg.wParam);
}

Window::~Window() {
    if (m_pDynamicLayout) {
        delete m_pDynamicLayout;
        m_pDynamicLayout = nullptr;
    }
}

void Window::Attach(HWND hwnd) {
    Detach();

    if (::IsWindow(hwnd)) {
        if (!WindowMap::Instance().GetWindow(hwnd)) {
            WindowMap::Instance().Add(hwnd, this);
            Subclass(hwnd);
        }
    }
}

HWND Window::Detach() {
    HWND hwnd = m_hwnd;
    if (m_pfnPrevWindowProc) {
        UnSubclass();
    }
    WindowMap::Instance().Remove(this);
    m_hwnd = nullptr;
    return hwnd;
}

void Window::Destroy() {
    ::DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
}

BOOL Window::IsWindow() const {
    return ::IsWindow(m_hwnd);
}

LRESULT Window::SendMessage(UINT message, WPARAM wParam, LPARAM lParam) const {
    return ::SendMessage(m_hwnd, message, wParam, lParam);
}

BOOL Window::Show(int cmd_show) const {
    return ::ShowWindow(m_hwnd, cmd_show);
}

HWND Window::GetHWND() const {
    return m_hwnd;
}

void Window::SetHWND(HWND hwnd) {
    m_hwnd = hwnd;
}

BOOL Window::SetPosition(HWND hwndInsertAfter, int x, int y, int w, int h, UINT flags) const {
    return ::SetWindowPos(m_hwnd, hwndInsertAfter, x, y, w, h, flags);
}

DWORD Window::GetStyle() const {
    return static_cast<DWORD>(::GetWindowLongPtr(m_hwnd, GWL_STYLE));
}

DWORD Window::SetStyle(DWORD dwStyle) const {
    return static_cast<DWORD>(::SetWindowLongPtr(m_hwnd, GWL_STYLE, static_cast<LONG_PTR>(dwStyle)));
}

BOOL Window::ModifyStyle(DWORD dwRemove, DWORD dwAdd, UINT nFlags) const {
    DWORD dwStyle = GetStyle();
    DWORD dwNewStyle = (dwStyle & ~dwRemove) | dwAdd;
    if (dwStyle == dwNewStyle) {
        return FALSE;
    }
    SetStyle(dwStyle);

    if (nFlags) {
        SetPosition(nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | nFlags);
    }
    return TRUE;
}

DWORD Window::GetExStyle() const {
    return static_cast<DWORD>(::GetWindowLongPtr(m_hwnd, GWL_EXSTYLE));
}

std::wstring Window::GetText() const {
    int len = ::GetWindowTextLength(m_hwnd);
    std::wstring buffer(len, L'\0');
    ::GetWindowText(m_hwnd, &buffer[0], len + 1);
    return buffer;
}

LRESULT Window::SetText(PCWSTR text) const {
    return SendMessage(WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text));
}

LRESULT Window::SetText(const std::wstring& text) const {
    return SetText(text.c_str());
}

int Window::GetTextLength() const {
    return ::GetWindowTextLength(m_hwnd);
}

DynamicLayout* Window::GetDynamicLayout() {
    return m_pDynamicLayout;
}

void Window::EnableDynamicLayout(bool enable) {
    if (m_pDynamicLayout) {
        delete m_pDynamicLayout;
        m_pDynamicLayout = nullptr;
    }

    if (!enable) {
        return;
    }

    m_pDynamicLayout = new DynamicLayout;
}

void Window::ResizeDynamicLayout() {
    if (m_pDynamicLayout && !::IsIconic(m_hwnd)) {
        if (!m_pDynamicLayout) {
            throw std::runtime_error("Invalid dynamic layout!");
        }
        m_pDynamicLayout->Adjust();
    }
}

BOOL Window::LoadDynamicLayoutResource(PCWSTR pszResourceName) {
    if (!IsWindow()) {
        return FALSE;
    }

    HINSTANCE hInstance = ::GetModuleHandle(nullptr);
    HRSRC hResource = ::FindResource(hInstance, pszResourceName, L"AFX_DIALOG_LAYOUT");
    if (!hResource) {
        ::MessageBox(nullptr, L"Dialog resource not found!", L"Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // Load resource data
    DWORD dwSize = 0;
    dwSize = SizeofResource(hInstance, hResource);
    HGLOBAL hGlobal = ::LoadResource(hInstance, hResource);
    if (!hGlobal) {
        ::MessageBox(nullptr, L"Failed to load dialog resource!", L"Error", MB_OK | MB_ICONERROR);
        ::FreeResource(hResource);
        return FALSE;
    }

    LPVOID pDialogTemplate = ::LockResource(hGlobal);
    if (!pDialogTemplate) {
        ::MessageBox(nullptr, L"Failed to lock dialog resource!", L"Error", MB_OK | MB_ICONERROR);
        UnlockResource(hResource);
        ::FreeResource(hResource);
        return FALSE;
    }

    // Load dialog template
    BOOL bResult = DynamicLayout::LoadResource(this, pDialogTemplate, dwSize);

    // Cleanup
    if (pDialogTemplate && hResource) {
        UnlockResource(hResource);
        ::FreeResource(hResource);
    }

    if (bResult) {
        InitDynamicLayout();
    }

    return bResult;
}

BOOL Window::InitDynamicLayout() {
    if (m_pDynamicLayout) {
        Dialog* dialog = dynamic_cast<Dialog*>(this);

        const bool is_child = (GetStyle() & WS_CHILD) == WS_CHILD;

        if (!is_child && dialog) {
            RECT rc;
            ::GetClientRect(m_hwnd, &rc);

            ModifyStyle(DS_MODALFRAME, WS_POPUP | WS_THICKFRAME);
            ::AdjustWindowRectEx(&rc, GetStyle(), ::IsMenu(GetMenu(m_hwnd)), GetExStyle());

            SetPosition(nullptr, 0, 0, RECTWIDTH(&rc), RECTHEIGHT(&rc), SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }
    else {
        throw std::logic_error("Invalid dynamic layout!");
    }
}

bool Window::IsDynamicLayoutEnabled() const {
    return !!m_pDynamicLayout;
}

LRESULT Window::DefaultWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        if (OnCommand(wParam, lParam)) {
            return 0;
        }
        break;

    case WM_CREATE:
        OnCreate(hwnd, reinterpret_cast<LPCREATESTRUCT>(lParam));
        break;

    case WM_DESTROY:
        if (OnDestroy()) {
            return 0;
        }
        break;

    case WM_SIZE: {
        SIZE size = { LOWORD(lParam, HIWORD(lParam)) };
        OnSize(message, static_cast<UINT>(wParam), size);
        break;
    }
    }

    if (m_pfnPrevWindowProc) {
        return ::CallWindowProc(m_pfnPrevWindowProc, hwnd, message, wParam, lParam);
    }
    else {
        return ::DefWindowProc(hwnd, message, wParam, lParam);
    }
}

LRESULT Window::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefaultWndProc(hwnd, message, wParam, lParam);
}

LRESULT Window::StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Window* window = WindowMap::Instance().GetWindow(hwnd);

    if (!window) {
        window = m_currentWindow;
        if (window) {
            window->SetHWND(hwnd);
            WindowMap::Instance().Add(hwnd, window);
        }
    }

    if (window) {
        return window->WndProc(hwnd, message, wParam, lParam);
    }
    else {
        return ::DefWindowProc(hwnd, message, wParam, lParam);
    }
}

BOOL Window::OnCommand(WPARAM wParam, LPARAM lParam) {
    return FALSE;
}

void Window::OnCreate(HWND hwnd, LPCREATESTRUCT lpcs) {
    LOGFONT lf;
    ::GetObject(::GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    m_hFont = ::CreateFontIndirect(&lf);
    ::SendMessage(m_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), FALSE);
}

BOOL Window::OnDestroy() {
    return FALSE;
}

void Window::OnSize(UINT message, UINT type, SIZE size) {
    ResizeDynamicLayout();
}

void Window::Subclass(HWND hwnd) {
    WNDPROC pfnCurrentProc = reinterpret_cast<WNDPROC>(::GetWindowLongPtr(hwnd, GWLP_WNDPROC));
    if (pfnCurrentProc != reinterpret_cast<WNDPROC>(StaticWndProc)) {
        m_pfnPrevWindowProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(StaticWndProc)));
        m_hwnd = hwnd;
    }
}

void Window::UnSubclass() {
    WNDPROC pfnCurrentProc = reinterpret_cast<WNDPROC>(::GetWindowLongPtr(m_hwnd, GWLP_WNDPROC));
    if (pfnCurrentProc == reinterpret_cast<WNDPROC>(StaticWndProc)) {
        ::SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_pfnPrevWindowProc));
        m_pfnPrevWindowProc = nullptr;
    }
}

Window* Window::m_currentWindow;

Dialog::~Dialog() {
    EndDialog(0);
}

INT_PTR Dialog::Create(UINT resource_id, HWND parent, bool modal) {
    m_resourceId = resource_id;

    if (modal) {
        INT_PTR result = ::DialogBoxParam(::GetModuleHandle(nullptr), MAKEINTRESOURCE(resource_id), parent, StaticDlgProc, reinterpret_cast<LPARAM>(this));
        m_hwnd = nullptr;
        return result;
    }
    else {
        m_hwnd = ::CreateDialogParam(::GetModuleHandle(nullptr), MAKEINTRESOURCE(resource_id), parent, StaticDlgProc, reinterpret_cast<LPARAM>(this));
        return reinterpret_cast<INT_PTR>(m_hwnd);
    }
}

void Dialog::EndDialog(INT_PTR result) {
    if (IsWindow()) {
        if (m_modal) {
            ::EndDialog(m_hwnd, result);
        }
        else {
            Destroy();
        }
    }
    m_hwnd = nullptr;
}

HWND Dialog::GetDlgItem(int id_item) const {
    return ::GetDlgItem(m_hwnd, id_item);
}

void Dialog::OnCancel() {
    EndDialog(IDCANCEL);
}

BOOL Dialog::OnClose() {
    return FALSE;
}

BOOL Dialog::OnInitDialog() {
    LoadDynamicLayoutResource(MAKEINTRESOURCE(m_resourceId));
    return TRUE;
}

void Dialog::OnOK() {
    EndDialog(IDOK);
}

INT_PTR Dialog::DlgProcDefault(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CLOSE: {
        return OnClose();
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDOK:
            OnOK();
            return TRUE;
        case IDCANCEL:
            OnCancel();
            return TRUE;
        default:
            return OnCommand(wParam, lParam);
        }
        break;
    }
    case WM_INITDIALOG:
        return OnInitDialog();

    case WM_SIZE: {
        SIZE size = { LOWORD(lParam), HIWORD(lParam) };
        OnSize(message, static_cast<UINT>(wParam), size);
        break;
    }
    }

    return FALSE;
}

INT_PTR Dialog::DlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    return DlgProcDefault(hDlg, message, wParam, lParam);
}

INT_PTR Dialog::StaticDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    Dialog* window = reinterpret_cast<Dialog*>(WindowMap::Instance().GetWindow(hDlg));

    if (!window && message == WM_INITDIALOG) {
        window = reinterpret_cast<Dialog*>(lParam);
        if (window) {
            window->SetHWND(hDlg);
            WindowMap::Instance().Add(hDlg, window);
        }
    }

    if (window) {
        return window->DlgProc(hDlg, message, wParam, lParam);
    }
    else {
        return FALSE;
    }
}


void DynamicLayoutData::CleanUp() {
    m_listCtrls.clear();
}

BOOL DynamicLayoutData::ReadResource(PVOID pResource, UINT nSize) {
    if (!(pResource && nSize)) {
        return FALSE;
    }

    CleanUp();

    const BYTE* const pBuf = (BYTE*)pResource;
    const WORD* const pwEnd = (WORD*)(pBuf + nSize);
    const WORD* pw = (WORD*)pBuf;

    // header
    WORD wVersion = *pw++;

    if (wVersion == 0) {
        // data
        while (pw + 4 <= pwEnd) {
            Item itemData;

            itemData.m_moveSettings.m_nXRatio = std::clamp(static_cast<int>(*pw++), 0, 100);
            itemData.m_moveSettings.m_nYRatio = std::clamp(static_cast<int>(*pw++), 0, 100);
            itemData.m_sizeSettings.m_nXRatio = std::clamp(static_cast<int>(*pw++), 0, 100);
            itemData.m_sizeSettings.m_nYRatio = std::clamp(static_cast<int>(*pw++), 0, 100);

            m_listCtrls.push_back(itemData);
        }

        return !m_listCtrls.empty();
    }

    return FALSE;
}

BOOL DynamicLayoutData::ApplyLayoutDataTo(Window* pHostWnd, BOOL bUpdate) {
    if (!pHostWnd->GetHWND() || m_listCtrls.empty()) {
        return FALSE;
    }

    if (!pHostWnd->IsWindow()) {
        throw std::runtime_error("ApplyLayoutDataTo: Window is invalid");
    }

    pHostWnd->EnableDynamicLayout(FALSE);
    pHostWnd->EnableDynamicLayout();

    auto pLayout = pHostWnd->GetDynamicLayout();
    if (!pLayout) {
        return FALSE;
    }

    if (!pLayout->Create(pHostWnd)) {
        return FALSE;
    }

    HWND hwndChild = ::GetWindow(pHostWnd->GetHWND(), GW_CHILD);
    for (auto& item : m_listCtrls) {
        if (!hwndChild) {
            break;
        }

        if (!item.m_moveSettings.IsNone() || !item.m_sizeSettings.IsNone()) {
            pLayout->AddItem(hwndChild, item.m_moveSettings, item.m_sizeSettings);
        }

        hwndChild = ::GetNextWindow(hwndChild, GW_HWNDNEXT);
    }

    if (bUpdate) {
        pLayout->Adjust();
    }

    return TRUE;
}

BOOL DynamicLayout::Create(Window* pHostWnd) {
    if (!pHostWnd->GetHWND()) {
        throw std::runtime_error("Failed to create dynamic layout for window");
        return FALSE;
    }

    m_pHostWnd = pHostWnd;
    return TRUE;
}

UINT DynamicLayout::AdjustItemRect(const DynamicLayoutItem& item, RECT& rc) {
    if (!m_pHostWnd || !::IsWindow(m_pHostWnd->GetHWND())) {
        throw std::runtime_error("Invalid host window");
    }

    ::SetRectEmpty(&rc);

    RECT rcHost;
    ::GetClientRect(m_pHostWnd->GetHWND(), &rcHost);

    if (rcHost.left == 0 && rcHost.right == 0 && rcHost.top == 0 && rcHost.bottom == 0) {
        return SWP_NOMOVE | SWP_NOSIZE;
    }

    UINT uiFlags = 0;
    const double deltaX = 0.01 * RECTWIDTH(&rcHost);
    const double deltaY = 0.01 * RECTHEIGHT(&rcHost);

    DynamicLayoutItem::Point point(item.m_ptInit);
    DynamicLayoutItem::Point size(item.m_szInit);

    // Is horizontal
    if (item.m_moveSettings.IsHorizontal()) {
        point.x += deltaX * item.m_moveSettings.m_nXRatio;
    }

    // Is vertical
    if (item.m_moveSettings.IsVertical()) {
        point.y += deltaY * item.m_moveSettings.m_nYRatio;
    }

    // Is horizontal
    if (item.m_sizeSettings.IsHorizontal()) {
        size.x += deltaX * item.m_sizeSettings.m_nXRatio;
    }

    if (item.m_sizeSettings.IsVertical()) {
        size.y += deltaY * item.m_sizeSettings.m_nYRatio;
    }

    rc.left = (long)point.x + rcHost.left;
    rc.top = (long)point.y + rcHost.top;
    rc.right = rc.left + (long)size.x;
    rc.bottom = rc.top + (long)size.y;

    if (rc.left == (item.m_ptInit.x + rcHost.left) && rc.top == (item.m_ptInit.y + rcHost.top)) {
        uiFlags |= SWP_NOMOVE;
    }

    if (RECTWIDTH(&rc) == item.m_szInit.x && RECTHEIGHT(&rc) == item.m_szInit.y) {
        uiFlags |= SWP_NOSIZE;
    }

    return uiFlags;
}

void DynamicLayout::Adjust() {
    if (m_listWnd.empty()) {
        return;
    }

    HDWP hdwp = ::BeginDeferWindowPos(static_cast<int>(m_listWnd.size()));

    for (auto& item : m_listWnd) {
        if (::IsWindow(item->hwnd)) {
            RECT rcItem;
            UINT uiFlags = AdjustItemRect(*item, rcItem);

            if ((uiFlags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
                ::DeferWindowPos(hdwp, item->hwnd, HWND_TOP, rcItem.left, rcItem.top,
                    RECTWIDTH(&rcItem), RECTHEIGHT(&rcItem),
                    uiFlags | SWP_NOZORDER | SWP_NOREPOSITION | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            }
        }
    }

    ::EndDeferWindowPos(hdwp);
}

BOOL DynamicLayout::AddItem(HWND hWnd, MoveSettings moveSettings, SizeSettings sizeSettings) {
    if (!(hWnd && ::IsWindow(hWnd) && ::IsChild(m_pHostWnd->GetHWND(), hWnd))) {
        throw;
        return FALSE;
    }

    // Item already exists
    DynamicLayoutItem* pItem = FindItem(hWnd);
    if (pItem) {
        throw;
        return FALSE;
    }

    pItem = new DynamicLayoutItem(hWnd, moveSettings, sizeSettings);
    if (PrepareItem(*pItem)) {
        m_listWnd.push_back(pItem);
    }

    return TRUE;
}

DynamicLayoutItem* DynamicLayout::FindItem(HWND hWnd) {
    for (auto& item : m_listWnd) {
        if (item->hwnd == hWnd) {
            return item;
        }
    }
    return nullptr;
}

BOOL DynamicLayout::PrepareItem(DynamicLayoutItem& item) const {
    RECT rcHost;
    ::GetClientRect(m_pHostWnd->GetHWND(), &rcHost);

    // Is rect null
    if (rcHost.left == 0 && rcHost.right == 0 && rcHost.top == 0 && rcHost.bottom == 0) {
        throw;
        return FALSE;
    }

    RECT rcChild = GetItemRect(item);

    const double deltaX = 0.01 * RECTWIDTH(&rcHost);
    const double deltaY = 0.01 * RECTHEIGHT(&rcHost);

    item.m_ptInit.x = (double)rcChild.left;
    item.m_ptInit.y = (double)rcChild.top;

    if (item.m_moveSettings.IsHorizontal()) {
        item.m_ptInit.x -= deltaX * item.m_moveSettings.m_nXRatio;
    }

    if (item.m_moveSettings.IsVertical()) {
        item.m_ptInit.y -= deltaY * item.m_moveSettings.m_nYRatio;
    }

    item.m_szInit.x = (double)RECTWIDTH(&rcChild);
    item.m_szInit.y = (double)RECTHEIGHT(&rcChild);

    if (item.m_sizeSettings.IsHorizontal()) {
        item.m_szInit.x -= deltaX * item.m_sizeSettings.m_nXRatio;
    }

    if (item.m_sizeSettings.IsVertical()) {
        item.m_szInit.y -= deltaY * item.m_sizeSettings.m_nYRatio;
    }

    return TRUE;
}

RECT DynamicLayout::GetItemRect(DynamicLayoutItem& item) const {
    RECT rcChild{};

    if (!m_pHostWnd) {
        throw;
        return RECT{};
    }

    ::GetWindowRect(item.hwnd, &rcChild);
    m_pHostWnd->ScreenToClient(&rcChild);

    return rcChild;
}

BOOL DynamicLayout::LoadResource(Window* pHostWnd, PVOID pResource, DWORD dwSize) {
    if (!(pHostWnd->GetHWND() && ::IsWindow(pHostWnd->GetHWND()) && pResource)) {
        return FALSE;
    }

    DynamicLayoutData layoutData;
    BOOL bResult = layoutData.ReadResource(pResource, (UINT)dwSize);
    layoutData.ApplyLayoutDataTo(pHostWnd, FALSE);

    return bResult;
}
