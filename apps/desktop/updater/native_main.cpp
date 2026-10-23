#include "native_strings.hpp"
#include <windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <shellapi.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
using orbelle::updater::native_text;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other) {
            close();
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~Handle() { close(); }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    void close() {
        if (*this)
            CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
};
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::wstring wide(const std::string &value) {
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    require(size > 0, "Invalid UTF-8 in managed file list.");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}
bool equal_path(const fs::path &a, const fs::path &b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool within(const fs::path &path, const fs::path &parent) {
    auto a = path.begin();
    for (auto b = parent.begin(); b != parent.end(); ++a, ++b)
        if (a == path.end() || !equal_path(*a, *b))
            return false;
    return true;
}
fs::path full_path(const fs::path &path) {
    const auto input = path.wstring();
    require(input.size() >= 3 && input[1] == L':' && (input[2] == L'\\' || input[2] == L'/'),
            "The installation must be an absolute local drive path.");
    require(input.find(L':', 2) == std::wstring::npos,
            "Alternate data streams are not installation paths.");
    std::vector<wchar_t> buffer(32768);
    const auto count =
        GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    require(count && count < buffer.size(), "Cannot resolve the installation path.");
    auto result = fs::path(buffer.data()).lexically_normal();
    while (!result.has_filename() && result != result.root_path())
        result = result.parent_path();
    return result;
}
fs::path own_path() {
    std::vector<wchar_t> value(32768);
    const auto count = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    require(count && count < value.size(), "Cannot locate Orbelle executable.");
    return full_path(value.data());
}
// Hold ancestors against replacement while validating/executing versioned programs.
std::vector<Handle> guard_directories(const fs::path &path) {
    std::vector<Handle> guards;
    fs::path current = path.root_path();
    for (const auto &part : path.relative_path()) {
        current /= part;
        Handle handle(CreateFileW(
            current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO info{};
        require(handle &&
                    GetFileInformationByHandleEx(handle.value, FileAttributeTagInfo, &info,
                                                 sizeof(info)) &&
                    (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "Installation directories must exist and must not contain reparse points.");
        guards.push_back(std::move(handle));
    }
    return guards;
}
Handle open_regular(const fs::path &path, DWORD access = GENERIC_READ,
                    DWORD share = FILE_SHARE_READ) {
    Handle handle(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    require(
        handle && GetFileInformationByHandle(handle.value, &info) &&
            !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            info.nNumberOfLinks == 1,
        "A managed file is missing, linked, or is not an ordinary file.");
    return handle;
}
std::string read_small(const fs::path &path, DWORD limit) {
    auto file = open_regular(path);
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.value, &size) && size.QuadPart >= 0 && size.QuadPart <= limit,
            "Managed metadata is too large.");
    std::string value(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    require(ReadFile(file.value, value.data(), static_cast<DWORD>(value.size()), &read, nullptr) &&
                read == value.size(),
            "Cannot read managed metadata.");
    return value;
}
bool version(const std::wstring &value) {
    if (value.empty() || value.size() > 32)
        return false;
    int parts = 0;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find(L'.', begin);
        const auto length = (end == std::wstring::npos ? value.size() : end) - begin;
        if (!length || length > 9 || (length > 1 && value[begin] == L'0'))
            return false;
        for (std::size_t i = begin; i < begin + length; ++i)
            if (value[i] < L'0' || value[i] > L'9')
                return false;
        ++parts;
        if (end == std::wstring::npos)
            return parts == 3;
        begin = end + 1;
    }
    return false;
}
std::wstring active_version(const fs::path &root) {
    const auto bytes = read_small(root / L"active-version.txt", 64);
    require(!bytes.empty() && bytes.back() == '\n', "Invalid active-version.txt.");
    auto result = wide(bytes.substr(0, bytes.size() - 1));
    require(version(result), "Invalid active version.");
    return result;
}
fs::path payload(const fs::path &root, const std::wstring &value) {
    require(version(value), "Invalid version: expected X.Y.Z.");
    return root / L"versions" / value;
}
std::wstring quote(const std::wstring &value) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (auto character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(backslashes * (character == L'\"' ? 2 : 1), L'\\');
        backslashes = 0;
        if (character == L'\"')
            result += L'\\';
        result += character;
    }
    result.append(backslashes * 2, L'\\');
    return result + L'\"';
}
Handle start(const fs::path &executable, const std::vector<std::wstring> &arguments) {
    auto directories = guard_directories(executable.parent_path());
    auto image = open_regular(executable, GENERIC_READ, FILE_SHARE_READ);
    auto command = quote(executable.wstring());
    for (const auto &argument : arguments)
        command += L" " + quote(argument);
    require(command.size() < 32767, "The command line is too long.");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    const auto standard_output = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto standard_error = GetStdHandle(STD_ERROR_HANDLE);
    const bool inherited_output = standard_output && standard_output != INVALID_HANDLE_VALUE &&
                                  standard_error && standard_error != INVALID_HANDLE_VALUE;
    if (inherited_output) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = standard_output;
        startup.hStdError = standard_error;
    }
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, inherited_output,
                           0, nullptr, nullptr, &startup, &process),
            "Cannot start Orbelle. Extract the complete application folder.");
    CloseHandle(process.hThread);
    return Handle(process.hProcess);
}
DWORD wait_exit(HANDLE process) {
    require(WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0, "Cannot wait for Orbelle.");
    DWORD code = 1;
    require(GetExitCodeProcess(process, &code), "Cannot obtain the Orbelle exit code.");
    return code;
}
Handle run_lock(const fs::path &root, DWORD timeout) {
    const auto deadline = GetTickCount64() + timeout;
    for (;;) {
        Handle result(CreateFileW((root / L".orbelle-running.lock").c_str(), GENERIC_READ, 0,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (result) {
            BY_HANDLE_FILE_INFORMATION info{};
            require(GetFileInformationByHandle(result.value, &info) &&
                        !(info.dwFileAttributes &
                          (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
                        info.nNumberOfLinks == 1,
                    "Invalid application lock file.");
            return result;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION || GetTickCount64() >= deadline)
            throw std::runtime_error(
                "Orbelle is already running, or its installation lock is unavailable.");
        Sleep(50);
    }
}
std::string sha256(HANDLE file) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0,
            "SHA-256 is unavailable.");
    struct CloseAlgorithm {
        BCRYPT_ALG_HANDLE value;
        ~CloseAlgorithm() { BCryptCloseAlgorithmProvider(value, 0); }
    } closer{algorithm};
    BCRYPT_HASH_HANDLE hash = nullptr;
    require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0,
            "Cannot create SHA-256 state.");
    struct CloseHash {
        BCRYPT_HASH_HANDLE value;
        ~CloseHash() { BCryptDestroyHash(value); }
    } hash_closer{hash};
    std::array<unsigned char, 65536> buffer{};
    DWORD count = 0;
    do {
        require(ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr),
                "Cannot verify a managed file.");
        require(BCryptHashData(hash, buffer.data(), count, 0) == 0, "Cannot hash a managed file.");
    } while (count);
    std::array<unsigned char, 32> digest{};
    require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0,
            "Cannot finish SHA-256.");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
std::string file_hash(const fs::path &path) {
    auto file = open_regular(path);
    return sha256(file.value);
}
bool hash_text(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
bool safe_relative(const fs::path &path) {
    if (path.empty() || path.is_absolute() || path.has_root_name())
        return false;
    for (const auto &part : path) {
        const auto value = part.wstring();
        if (value.empty() || value == L"." || value == L".." || value.back() == L'.' ||
            value.back() == L' ' || value.find_first_of(L":\\*?\"<>|") != std::wstring::npos)
            return false;
    }
    return true;
}
void cleanup_version(const fs::path &folder) {
    // Only complete, unchanged, explicitly managed payloads are eligible. Unexpected empty
    // directories count as user content too. Never recursively remove an arbitrary directory.
    if (within(full_path(fs::current_path()), folder))
        return;
    std::vector<wchar_t> environment(32768);
    const auto length = GetEnvironmentVariableW(L"ORBELLE_DATA_HOME", environment.data(),
                                                static_cast<DWORD>(environment.size()));
    if (length && (length >= environment.size() || within(full_path(environment.data()), folder)))
        return;
    auto guards = guard_directories(folder);
    const auto marker = read_small(folder / L"orbelle-managed.txt", 80);
    require(marker.size() == 67 && marker.substr(0, 2) == "1\n" && marker.back() == '\n' &&
                hash_text(marker.substr(2, 64)),
            "Invalid managed version marker.");
    require(file_hash(folder / L"orbelle-managed-files.txt") == marker.substr(2, 64),
            "Managed file list has changed.");
    const auto listing = read_small(folder / L"orbelle-managed-files.txt", 16 * 1024 * 1024);
    std::map<fs::path, std::string> expected;
    std::set<fs::path> directories;
    std::size_t begin = 0;
    while (begin < listing.size()) {
        const auto end = listing.find('\n', begin);
        require(end != std::string::npos && end > begin + 66, "Invalid managed file list line.");
        const auto hash = listing.substr(begin, 64);
        const auto relative_bytes = listing.substr(begin + 66, end - begin - 66);
        const auto relative = fs::path(wide(relative_bytes));
        require(hash_text(hash) && listing.substr(begin + 64, 2) == "  " &&
                    relative_bytes.find('\\') == std::string::npos &&
                    relative_bytes.find('\0') == std::string::npos && safe_relative(relative) &&
                    relative != L"orbelle-managed.txt" &&
                    relative != L"orbelle-managed-files.txt" &&
                    expected.emplace(relative, hash).second,
                "Unsafe or duplicate managed file path.");
        for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path())
            directories.insert(parent);
        begin = end + 1;
    }
    require(expected.contains(L"OrbelleApp.exe") && expected.contains(L"OrbelleUpdater.exe") &&
                expected.contains(L"OrbelleLauncher.exe") && expected.contains(L"manifest.json"),
            "Incomplete managed version.");
    // OPEN_REPARSE_POINT protects only the final component. Pin every intermediate
    // directory too, before traversal or opening files, so it cannot be replaced with
    // a junction between validation and deletion. Each guard also checks its ancestors.
    std::vector<std::vector<Handle>> internal_guards;
    for (const auto &relative : directories)
        internal_guards.push_back(guard_directories(folder / relative));
    auto remaining = expected;
    auto remaining_directories = directories;
    for (const auto &entry : fs::recursive_directory_iterator(folder)) {
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES &&
                    !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY)),
                "A managed directory contains a reparse point or protected file.");
        const auto relative = entry.path().lexically_relative(folder);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            require(remaining_directories.erase(relative) == 1,
                    "Unknown directory: preserving old version.");
        } else if (relative != L"orbelle-managed.txt" && relative != L"orbelle-managed-files.txt") {
            const auto found = remaining.find(relative);
            require(found != remaining.end() && file_hash(entry.path()) == found->second,
                    "Unknown or modified file: preserving old version.");
            remaining.erase(found);
        }
    }
    require(remaining.empty() && remaining_directories.empty(), "Managed version is incomplete.");
    // All validation finishes before the first deletion. File-level deletion and empty-only
    // directory removal preserve content added concurrently, without traversing it.
    std::vector<Handle> files;
    for (const auto &[relative, hash] : expected) {
        auto file = open_regular(folder / relative, GENERIC_READ | DELETE, FILE_SHARE_READ);
        require(sha256(file.value) == hash, "Managed file changed before cleanup.");
        files.push_back(std::move(file));
    }
    for (auto &file : files) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        require(SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition,
                                           sizeof(disposition)),
                "Cannot remove an old program file.");
        file.close();
    }
    for (const auto *name : {L"orbelle-managed.txt", L"orbelle-managed-files.txt"})
        require(DeleteFileW((folder / name).c_str()), "Cannot remove managed cleanup metadata.");
    std::vector<fs::path> ordered(directories.begin(), directories.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) { return a.native().size() > b.native().size(); });
    internal_guards.clear();
    for (const auto &relative : ordered)
        RemoveDirectoryW((folder / relative).c_str());
    guards.clear();
    RemoveDirectoryW(folder.c_str());
}
void cleanup_old_versions(const fs::path &root, const std::wstring &current) noexcept {
    try {
        for (const auto &entry : fs::directory_iterator(root / L"versions")) {
            const auto name = entry.path().filename().wstring();
            if (version(name) && name != current) {
                try {
                    cleanup_version(entry.path());
                } catch (...) {
                    // A locked, modified, or user-owned directory is not an update failure.
                }
            }
        }
    } catch (...) {
    }
}
void publish_version(const fs::path &root, const std::wstring &value) {
    const auto destination = root / L"active-version.txt";
    auto existing = open_regular(destination, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE);
    const auto pending =
        root / (L"active-version.pending-" + std::to_wstring(GetCurrentProcessId()));
    Handle file(CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    require(static_cast<bool>(file), "Cannot prepare the new active version.");
    std::string content;
    for (const auto character : value)
        content += static_cast<char>(character); // value was validated as ASCII X.Y.Z.
    const auto bytes = content + "\n";
    DWORD written = 0;
    const bool written_ok =
        WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file.value);
    file.close();
    bool replaced = false;
    if (written_ok) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (ReplaceFileW(destination.c_str(), pending.c_str(), nullptr, 0, nullptr, nullptr)) {
                replaced = true;
                break;
            }
            const auto error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
                break;
            Sleep(20);
        }
    }
    if (!replaced) {
        DeleteFileW(pending.c_str());
        throw std::runtime_error(
            "Cannot publish the new version; the previous version remains active.");
    }
}
void publish_launcher(const fs::path &root, const fs::path &next_folder) {
    auto source = open_regular(next_folder / L"OrbelleLauncher.exe");
    const auto destination = root / L"Orbelle.exe";
    auto existing =
        open_regular(destination, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE);
    const auto pending = root / (L"Orbelle.pending-" + std::to_wstring(GetCurrentProcessId()));
    Handle output(CreateFileW(pending.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
    require(static_cast<bool>(output), "Cannot prepare the updated launcher.");
    try {
        std::array<unsigned char, 65536> buffer{};
        DWORD count = 0;
        do {
            require(ReadFile(source.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                             nullptr),
                    "Cannot read the updated launcher.");
            DWORD written = 0;
            require(WriteFile(output.value, buffer.data(), count, &written, nullptr) &&
                        written == count,
                    "Cannot write the updated launcher.");
        } while (count);
        require(FlushFileBuffers(output.value), "Cannot flush the updated launcher.");
        const auto name = destination.wstring();
        const auto bytes = sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t);
        std::vector<std::uintptr_t> storage((bytes + sizeof(std::uintptr_t) - 1) /
                                            sizeof(std::uintptr_t));
        auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        rename->ReplaceIfExists = TRUE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::copy(name.begin(), name.end(), rename->FileName);
        existing.close(); // Windows requires the replaced destination to have no open handles.
        // Rename the held temporary file itself, leaving no path-replacement window between
        // verification and publication. Protocol-1 launchers must also run the old payload.
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (SetFileInformationByHandle(output.value, FileRenameInfo, rename,
                                           static_cast<DWORD>(bytes)))
                return;
            const auto error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
                break;
            Sleep(20);
        }
        throw std::runtime_error(
            "Cannot replace the launcher; the previous version remains active.");
    } catch (...) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(output.value, FileDispositionInfo, &disposition,
                                   sizeof(disposition));
        throw;
    }
}
void message(const wchar_t *value) {
    MessageBoxW(nullptr, value, native_text("updates.native.title"),
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}
BOOL CALLBACK close_process_window(HWND window, LPARAM process_id) {
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(process_id) && IsWindowVisible(window))
        PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}
DWORD apply(const std::vector<std::wstring> &arguments) {
    require((arguments.size() == 5 || arguments.size() == 7) && arguments[1] == L"--apply-update" &&
                (arguments.size() == 5 || arguments[5] == L"--parent-ready-event"),
            "Invalid updater arguments.");
    const auto root = full_path(arguments[2]);
    auto root_guards = guard_directories(root);
    require(read_small(root / L"orbelle-layout.txt", 16) == "1\n",
            "Unsupported Orbelle installation layout.");
    const auto next = arguments[3];
    const auto next_folder = payload(root, next);
    require(equal_path(own_path(), next_folder / L"OrbelleUpdater.exe"),
            "Run the verified updater from the new version directory.");
    const auto previous = active_version(root);
    require(next != previous, "The requested version is already active.");
    require(!arguments[4].empty() && arguments[4].size() <= 10 &&
                std::all_of(arguments[4].begin(), arguments[4].end(),
                            [](wchar_t c) { return c >= L'0' && c <= L'9'; }),
            "Invalid parent process ID.");
    const auto parsed_pid = std::stoull(arguments[4]);
    require(parsed_pid > 0 && parsed_pid <= MAXDWORD, "Invalid parent process ID.");
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                              static_cast<DWORD>(parsed_pid)));
    require(static_cast<bool>(parent), "Cannot hold the running Orbelle process.");
    std::vector<wchar_t> image(32768);
    DWORD image_size = static_cast<DWORD>(image.size());
    require(QueryFullProcessImageNameW(parent.value, 0, image.data(), &image_size) &&
                equal_path(full_path(image.data()), payload(root, previous) / L"OrbelleApp.exe"),
            "The parent process is not this installation's active Orbelle application.");
    auto next_guards = guard_directories(next_folder);
    {
        auto file = open_regular(next_folder / L"OrbelleApp.exe");
    }
    Handle cancelled;
    if (arguments.size() == 7) {
        require(arguments[6].starts_with(L"Local\\Orbelle.UpdateParent.") &&
                    arguments[6].size() < 200,
                "Invalid parent readiness event.");
        Handle prepared(OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[6].c_str()));
        cancelled = Handle(OpenEventW(SYNCHRONIZE, FALSE, (arguments[6] + L".cancel").c_str()));
        require(prepared && cancelled && SetEvent(prepared.value),
                "Cannot acknowledge updater preparation.");
    }
    // Parent may cancel/decline closing. Waiting does not alter the installation or data.
    if (cancelled) {
        const HANDLE events[]{cancelled.value, parent.value};
        const auto result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0)
            return 0;
        require(result == WAIT_OBJECT_0 + 1, "Cannot wait for the original application to exit.");
    } else {
        wait_exit(parent.value);
    }
    parent.close();
    auto lock = run_lock(root, 15000);
    require(active_version(root) == previous, "Another update changed the active version.");
    std::array<unsigned char, 16> nonce{};
    require(BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
            "Cannot create a startup nonce.");
    std::wstring event_name =
        L"Local\\Orbelle.Ready." + std::to_wstring(GetCurrentProcessId()) + L".";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (auto byte : nonce) {
        event_name += hex[byte >> 4];
        event_name += hex[byte & 15];
    }
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    require(ready && GetLastError() != ERROR_ALREADY_EXISTS,
            "Cannot create startup readiness event.");
    Handle child;
    bool success = false;
    try {
        child = start(next_folder / L"OrbelleApp.exe", {L"--update-ready-event", event_name});
        const HANDLE events[]{child.value, ready.value};
        const auto result = WaitForMultipleObjects(2, events, FALSE, 120000);
        if (result == WAIT_OBJECT_0 + 1 && WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT) {
            publish_launcher(root, next_folder);
            publish_version(root, next);
            success = true;
        }
    } catch (...) {
        // Roll back by keeping the original pointer; the old payload was never overwritten.
    }
    if (success) {
        cleanup_old_versions(root, next);
        return wait_exit(child.value);
    }
    if (child && WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT) {
        EnumWindows(close_process_window, static_cast<LPARAM>(GetProcessId(child.value)));
        if (WaitForSingleObject(child.value, 15000) != WAIT_OBJECT_0) {
            message(native_text("updates.native.close_new"));
            wait_exit(child.value);
        }
    }
    message(native_text("updates.native.rollback"));
    auto old = start(payload(root, previous) / L"OrbelleApp.exe", {});
    return wait_exit(old.value);
}
DWORD launch(const std::vector<std::wstring> &arguments) {
    const auto root = own_path().parent_path();
    auto guards = guard_directories(root);
    require(read_small(root / L"orbelle-layout.txt", 16) == "1\n",
            "Unsupported Orbelle installation layout.");
    auto lock = run_lock(root, 0);
    const auto selected = active_version(root);
    const auto folder = payload(root, selected);
    auto payload_guards = guard_directories(folder);
    std::vector<std::wstring> forwarded(arguments.begin() + 1, arguments.end());
    auto child = start(folder / L"OrbelleApp.exe", forwarded);
    // The launcher owns the run lock until its child exits. The updater then takes it over.
    return wait_exit(child.value);
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto **values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values)
        return 1;
    std::vector<std::wstring> arguments(values, values + count);
    LocalFree(values);
    try {
#ifdef ORBELLE_UPDATE_HELPER
        return static_cast<int>(apply(arguments));
#else
        return static_cast<int>(launch(arguments));
#endif
    } catch (const std::exception &error) {
        const auto detail =
            std::wstring(native_text("updates.native.failure")) + wide(error.what());
        message(detail.c_str());
        return 1;
    }
}
