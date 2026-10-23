#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>

int wmain(int argc, wchar_t **argv) {
    std::wstring event;
    int code = 0, lifetime = 100, startup = 0, signal = 1;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    const auto config = std::filesystem::path(executable).parent_path() / L"stub.cfg";
    std::ifstream input(config);
    if (input)
        input >> signal >> startup >> lifetime >> code;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--update-ready-event" && i + 1 < argc)
            event = argv[++i];
        else if (argument == L"--record" && i + 1 < argc) {
            std::ofstream record(std::filesystem::path(argv[++i]), std::ios::binary);
            for (int j = i + 1; j < argc; ++j) {
                const std::wstring value = std::wstring(argv[j]) + L"\n";
                record.write(reinterpret_cast<const char *>(value.data()),
                             static_cast<std::streamsize>(value.size() * sizeof(wchar_t)));
            }
        }
    }
    Sleep(static_cast<DWORD>(startup));
    if (signal && !event.empty()) {
        const auto ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, event.c_str());
        if (!ready)
            return 90;
        SetEvent(ready);
        CloseHandle(ready);
    }
    Sleep(static_cast<DWORD>(lifetime));
    return code;
}
