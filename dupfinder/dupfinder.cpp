#include <Windows.h>
#include "resource.h"
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <Shlwapi.h>
#include <Uxtheme.h>
#include <iostream>
#include <filesystem>
#include <unordered_map>
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
#include <mutex>
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
std::unordered_map<std::wstring, std::vector<fs::path>> find_duplicate_files(const fs::path& root, std::function<void(std::wstring)> logCallback = [](std::wstring){}) {
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

const WCHAR szClassName[] = L"WindowClass";

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

HWND g_hEditFolderPath;
HWND g_hListResults;

HIMAGELIST hImageList;

void InitListView(HWND hwndListView) {
    
    // Enable Explorer-style theme
    SetWindowTheme(hwndListView, L"Explorer", NULL);

    // Enable grouping in the ListView
    ListView_SetExtendedListViewStyle(hwndListView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ::SendMessage(hwndListView, LVM_ENABLEGROUPVIEW, TRUE, 0);

    hImageList = ImageList_Create(96, 96, ILC_COLOR32 | ILC_MASK, 1, 1);
    ListView_SetImageList(hwndListView, hImageList, LVSIL_NORMAL);

    const int horizontalSpacing = 128;
    const int verticalSpacing = 128;
    ListView_SetIconSpacing(hwndListView, horizontalSpacing, verticalSpacing);

    LVCOLUMN lvc{};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.pszText = const_cast<PWSTR>(L"Name");
    lvc.cx = 640;
    ListView_InsertColumn(hwndListView, 0, &lvc);
}

int g_groupId = 1;

int CreateDuplicateGroup(HWND hwndListView, std::wstring hash) {
    LVGROUP group;
    group.cbSize = sizeof(LVGROUP);
    group.mask = LVGF_HEADER | LVGF_GROUPID;
    group.pszHeader = const_cast<PWSTR>(hash.c_str());
    group.iGroupId = g_groupId + 1;
    ::SendMessage(hwndListView, LVM_INSERTGROUP, 0, (LPARAM)&group);

    ++g_groupId;
    return group.iGroupId;
}


int g_item;
void AddDuplicateFileItem(HWND hwndListView, std::wstring path, int iGroupId) {

    SIZE size = { 96, 96 };
    HBITMAP hBitmap = GetThumbnail(path, size);

    if (hBitmap) {
        // Add the thumbnail to the ImageList
        int imageIndex = ImageList_Add(hImageList, hBitmap, nullptr);
        ::DeleteObject(hBitmap);

        // Add item to Group 1
        LVITEM item{};
        item.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_IMAGE;
        item.iItem = ListView_GetItemCount(hwndListView);
        item.pszText = const_cast<PWSTR>(path.c_str());
        item.iGroupId = iGroupId;
        item.iImage = imageIndex;
        ListView_InsertItem(hwndListView, &item);
    }
}

void AddItemsToListView(HWND hwndListView) {
    
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

struct OverlappedContext : OVERLAPPED {
    OVERLAPPED overlapped;
    fs::path dir;
    DirectoryEntry dirEntry;
    std::function<void(fs::path)> callback;
    char buffer[1024];
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


void WatchDirectory(const std::wstring& directoryPath, const std::wstring& fileName) {
    HANDLE hDirectory = ::CreateFile(
        directoryPath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (hDirectory == INVALID_HANDLE_VALUE) {
        ::MessageBox(nullptr, L"Failed to open directory handle.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    char buffer[1024];
    DWORD bytesReturned;

    while (true) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = hDirectory;

        
    }

    CloseHandle(hDirectory);
}

void ShowShellContextMenu(HWND hWnd, const std::wstring& filePath, POINT pt) {
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
                    int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);

                    if (cmd > 0) {
                        if (cmd >= 0x8000 && cmd <= 0x8001) {
                            // Handle custom actions
                            if (cmd == 0x8000) {
                                OpenFileLocation(filePath);
                            }
                            else if (cmd == 0x8001) {
                                MessageBox(hWnd, L"Custom Action 2 executed!", L"Custom Menu", MB_OK);
                            }
                        }
                        else {
                            // Execute the shell command
                            // Execute the selected command
                            CMINVOKECOMMANDINFOEX cmi{};
                            cmi.cbSize = sizeof(cmi);
                            cmi.fMask = CMIC_MASK_UNICODE;
                            cmi.hwnd = hWnd;
                            cmi.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                            cmi.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                            cmi.nShow = SW_NORMAL;

                            hr = pContextMenu->InvokeCommand((LPCMINVOKECOMMANDINFO)&cmi);

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

FileWatcher g_fileWatcher;

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:

        g_hEditFolderPath = ::GetDlgItem(hDlg, IDC_EDIT1);
        g_hListResults = ::GetDlgItem(hDlg, IDC_LIST1);
        InitListView(g_hListResults);
        AddItemsToListView(g_hListResults);

        g_fileWatcher.SetCallback([=](fs::path path) -> void {
            // Find the item in the ListView
            LVFINDINFO findInfo{};
            findInfo.flags = LVFI_STRING;

            std::wstring wpath = path.wstring();
            findInfo.psz = wpath.c_str();

            int index = ListView_FindItem(g_hListResults, -1, &findInfo);
            if (index != -1) {
                ListView_DeleteItem(g_hListResults, index);
            }
        });

        return TRUE;
        break;

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
                    wchar_t filePath[MAX_PATH]{};
                    ListView_GetItemText(g_hListResults, index, 0, filePath, MAX_PATH);

                    // Show shell context menu
                    ShowShellContextMenu(hDlg, filePath, pt);
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

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
        case IDCANCEL:
            ::DestroyWindow(hDlg);
            return TRUE;
            break;

        case IDC_BUTTON1:
            if (HIWORD(wParam) == BN_CLICKED) {

                auto selectedFolder = ::SelectDirectory(hDlg);
                if (!selectedFolder.empty()) {
                    ::SetWindowText(g_hEditFolderPath, selectedFolder.c_str());
                }
                return TRUE;
                break;
            }
            break;

        case IDC_BUTTON2:
            if (HIWORD(wParam) == BN_CLICKED) {
                int len = ::GetWindowTextLength(g_hEditFolderPath);
                std::wstring selectedFolder(len, L'\0');
                ::GetDlgItemText(hDlg, IDC_EDIT1, &selectedFolder[0], len + 1);
                selectedFolder.resize(wcslen(selectedFolder.c_str()));

                HWND hEditLog = ::GetDlgItem(hDlg, IDC_EDIT2);

                auto duplicates = find_duplicate_files(selectedFolder, [&](std::wstring message) {
                    int len = ::GetWindowTextLength(hEditLog);
                    ::SendMessage(hEditLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
                    ::SendMessage(hEditLog, EM_REPLACESEL, FALSE, (LPARAM)message.c_str());
                    });

                for (const auto& [hash, files] : duplicates) {
                    std::wcout << L"Duplicate files (Hash: " << hash << L"):\n";


                    int groupId = CreateDuplicateGroup(g_hListResults, hash);

                    for (const auto& file : files) {

                        g_fileWatcher.AddFile(file);

                        // try {
                        //     
                        // }
                        // catch (std::exception& e) {
                        // 
                        // }

                        
                        AddDuplicateFileItem(g_hListResults, file, groupId);
                    }
                }


                return TRUE;
                break;
            }
            break;

        case ID_VIEW_ICONS:
            ListView_SetView(g_hListResults, LV_VIEW_ICON);
            return TRUE;
            break;

        case ID_VIEW_LIST:
            ListView_SetView(g_hListResults, LV_VIEW_LIST);
            return TRUE;
            break;

        case ID_VIEW_DETAILS:
            ListView_SetView(g_hListResults, LV_VIEW_DETAILS);
            return TRUE;
            break;
        }

        return FALSE;
        break;

        /*
    case WM_CONTEXTMENU:
        if ((HWND)wParam == g_hListResults) {
            // GetCursorPosition
            POINT pt;
            ::GetCursorPos(&pt);

            // Map the point to ListView coordinates
            ::ScreenToClient(g_hListResults, &pt);

            // Determine the clicked item
            LVHITTESTINFO hitTestInfo{};
            hitTestInfo.pt = pt;
            int index = ListView_HitTest(g_hListResults, &hitTestInfo);

            if (index != -1 && (hitTestInfo.flags & LVHT_ONITEM)) {
                // Load or create the context menu
                HMENU hMenu = ::LoadMenu(::GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_MENU1));
                if (hMenu) {
                    HMENU hSubMenu = GetSubMenu(hMenu, 1);

                    // Display the context menu
                    ::ClientToScreen(g_hListResults, &pt);
                    ::TrackPopupMenu(hSubMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hDlg, nullptr);

                    // Cleanup
                    DestroyMenu(hMenu);
                }
            }
        }
        return TRUE;
        break;
        */

    case WM_SIZE: {
        RECT rcClient;
        ::GetClientRect(hDlg, &rcClient);

        HDWP hdwp = ::BeginDeferWindowPos(1);
        // hdwp = ::DeferWindowPos(hdwp, g_hListView, HWND_TOP, 0, 0, RECTWIDTH(&rcClient), RECTHEIGHT(&rcClient) - 40, SWP_SHOWWINDOW);

        HINSTANCE hInstance = ::GetModuleHandle(nullptr);

        HRSRC hResource = ::FindResource(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), RT_DIALOG);
        if (!hResource) {
            MessageBox(nullptr, L"Dialog resource not found!", L"Error", MB_OK);
            return FALSE;
        }

        // Load resource data
        HGLOBAL hGlobal = LoadResource(hInstance, hResource);
        if (!hGlobal) {
            MessageBox(nullptr, L"Failed to load dialog resource!", L"Error", MB_OK);
            return FALSE;
        }

        // Lock the resource to access the template data
        LPVOID lpDialogTemplate = ::LockResource(hGlobal);
        if (!lpDialogTemplate) {
            MessageBox(nullptr, L"Failed to lock dialog resource!", L"Error", MB_OK);
            return FALSE;
        }

        // Pointer to the dialog template structure
        DLGTEMPLATEEX dlgTemplate;
        void* head = ReadDlgTemplateEx(lpDialogTemplate, &dlgTemplate);

        // Get the width and height of the dialog
        int dlgWidth = dlgTemplate.cx;
        int dlgHeight = dlgTemplate.cy;

        int controlCount = dlgTemplate.cDlgItems;

        RECT rcDialog{};
        rcDialog.left = 0;
        rcDialog.top = 0;
        rcDialog.right = dlgWidth;
        rcDialog.bottom = dlgHeight;
        MapDialogRect(hDlg, &rcDialog);

        // Loop through the controls and get their position and size
        DLGITEMTEMPLATEEX item;
        for (int i = 0; i < controlCount; i++) {
            head = ReadDlgItemTemplateEx(head, &item);

            int controlX = item.x;
            int controlY = item.y;
            int controlWidth = item.cx;
            int controlHeight = item.cy;

            RECT rc{};
            rc.left = controlX;
            rc.top = controlY;
            rc.right = controlX + controlWidth;
            rc.bottom = controlY + controlHeight;
            MapDialogRect(hDlg, &rc);

            HWND hDlgItem = ::GetDlgItem(hDlg, item.id);

            switch (item.id) {
            case IDC_LIST1:
            {
                hdwp = ::DeferWindowPos(hdwp, hDlgItem, HWND_TOP,
                    rc.left,
                    rc.top,
                    RECTWIDTH(&rcClient) - rc.left - (RECTWIDTH(&rcDialog) - rc.right),
                    RECTHEIGHT(&rcClient) - rc.top - (RECTHEIGHT(&rcDialog) - rc.bottom),
                    SWP_NOREPOSITION);
            }
            break;

            case IDC_EDIT2:
            {
                hdwp = ::DeferWindowPos(hdwp, hDlgItem, HWND_TOP,
                    rc.left,
                    RECTHEIGHT(&rcClient) - (RECTHEIGHT(&rcDialog) - rc.top),
                    RECTWIDTH(&rcClient) - rc.left - (RECTWIDTH(&rcDialog) - rc.right),
                    RECTHEIGHT(&rc),
                    SWP_NOREPOSITION);
            }
            break;

            case IDC_PROGRESS1:
            {
                hdwp = ::DeferWindowPos(hdwp, hDlgItem, HWND_TOP,
                    rc.left,
                    RECTHEIGHT(&rcClient) - (RECTHEIGHT(&rcDialog) - rc.top),
                    RECTWIDTH(&rcClient) - rc.left - (RECTWIDTH(&rcDialog) - rc.right),
                    RECTHEIGHT(&rc),
                    SWP_NOREPOSITION);
            }
            break;

            case IDOK:
            case IDCANCEL:
            {
                hdwp = ::DeferWindowPos(hdwp, hDlgItem, HWND_TOP,
                    RECTWIDTH(&rcClient) - (RECTWIDTH(&rcDialog) - rc.left),
                    RECTHEIGHT(&rcClient) - (RECTHEIGHT(&rcDialog) - rc.top),
                    RECTWIDTH(&rc), RECTHEIGHT(&rc), SWP_NOSIZE);
            }
            break;
            }
        }

        // RECT rcWindow;
        // ::GetWindowRect(g_hBtnOk, &rcWindow);
        // hdwp = ::DeferWindowPos(hdwp, g_hBtnOk, HWND_TOP, RECTWIDTH(&rcClient) - RECTWIDTH(&rcWindow) - 11, RECTHEIGHT(&rcClient) - RECTHEIGHT(&rcWindow) - 11, 300, 26, SWP_SHOWWINDOW | SWP_NOSIZE);
        ::EndDeferWindowPos(hdwp);
        return TRUE;
    }
        break;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return FALSE;
        break;
    }

    

    return FALSE;
}

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

    MSG msg{};

    std::locale::global(std::locale("en_US.UTF-8"));

    INITCOMMONCONTROLSEX iccex{};
    iccex.dwSize = sizeof(iccex);
    iccex.dwICC = ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&iccex);

    try {
        std::thread fileWatcherThread(std::ref(g_fileWatcher));
        fileWatcherThread.detach();

        HWND hDialog = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), nullptr, reinterpret_cast<DLGPROC>(DlgProc));
        ShowWindow(hDialog, nCmdShow);
    
        while (::GetMessage(&msg, nullptr, 0, 0)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
    }
    catch (std::exception& e) {
        std::wstring message = CharToWChar(e.what());
        ::MessageBox(nullptr, message.c_str(), nullptr, MB_OK | MB_ICONERROR);
    }

    return static_cast<int>(msg.wParam);
}

int main() {
    fs::path directory_to_scan = L"D:\\WebM"; // Change to your directory
    fs::path duplicates_folder = L"./duplicates";

    try {
        // Set the locale to handle Unicode properly
        std::locale::global(std::locale("en_US.UTF-8"));

        // Create the duplicates folder if it doesn't exist
        if (!fs::exists(duplicates_folder)) {
            fs::create_directory(duplicates_folder);
        }

        auto duplicates = find_duplicate_files(directory_to_scan);

        for (const auto& [hash, files] : duplicates) {
            std::wcout << L"Duplicate files (Hash: " << hash << L"):\n";

            // Move the first file to the duplicates folder
            auto first_file = files.front();
            fs::path destination = duplicates_folder / first_file.filename();

            // Ensure unique naming if file already exists in duplicates folder
            int counter = 1;
            while (fs::exists(destination)) {
                destination = duplicates_folder / (first_file.stem().wstring() + L"_" + std::to_wstring(counter) + first_file.extension().wstring());
                ++counter;
            }

            try {
                fs::copy(first_file, destination, fs::copy_options::overwrite_existing);
                std::wcout << L"  Copied: " << first_file.wstring() << L" -> " << destination.wstring() << L'\n';
            }
            catch (const std::exception& e) {
                std::wcerr << L"  Error copying " << first_file.wstring() << L": " << e.what() << L'\n';
                continue;
            }

            // Delete all duplicate files (including the first one from original location)
            for (const auto& file : files) {
                try {
                    fs::remove(file);
                    std::wcout << L"  Deleted: " << file.wstring() << L'\n';
                }
                catch (const std::exception& e) {
                    std::wcerr << L"  Error deleting " << file.wstring() << L": " << e.what() << L'\n';
                }
            }
        }

        if (duplicates.empty()) {
            std::wcout << L"No duplicate files found.\n";
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Error: " << e.what() << L'\n';
    }

    return 0;
}
