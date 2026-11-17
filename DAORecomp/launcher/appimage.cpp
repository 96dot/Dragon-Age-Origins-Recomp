#include "stdafx.h"
#include "launcher/appimage.h"

#include <fstream>

#include "files.h"
#include "log.h"
#include "program.h"
#include "text.h"

namespace fs = std::filesystem;

namespace dao::launcher::appimage
{
    namespace
    {
        std::wstring Env(const wchar_t* name)
        {
            const wchar_t* value = _wgetenv(name);
            return value ? std::wstring(value) : std::wstring();
        }

        fs::path DataFolder()
        {
            return Env(L"DAO_APPIMAGE_DATA");
        }

        // `path`'s Linux path, as Wine maps it (Z:\home\... -> /home/...); empty if
        // Wine can't say. (Wine's kernel32 exports it, Proton's too; not ntdll.)
        std::string UnixPath(const fs::path& path)
        {
            using Fn = char*(CDECL*)(LPCWSTR);
            static const auto unixName = reinterpret_cast<Fn>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name"));
            if (!unixName)
                return {};
            char* name = unixName(path.c_str());
            if (!name)
                return {};
            std::string out = name;
            HeapFree(GetProcessHeap(), 0, name);
            return out;
        }
    }

    bool Active()
    {
        return IsUnderWine() && !Env(L"DAO_APPIMAGE").empty() && !DataFolder().empty();
    }

    std::string AppImagePath()
    {
        return Active() ? WideToUtf8(Env(L"DAO_APPIMAGE")) : std::string();
    }

    std::string IconPath()
    {
        return Active() ? WideToUtf8(Env(L"DAO_APPIMAGE_ICON")) : std::string();
    }

    fs::path HostSteam()
    {
        if (!Active())
            return {};
        const fs::path steam = Env(L"DAO_HOST_STEAM");
        std::error_code ec;
        return !steam.empty() && fs::is_directory(steam / "userdata", ec) ? steam : fs::path();
    }

    void RecordInstall(const fs::path& exe)
    {
        if (!Active())
            return;
        // The Linux path, which the AppImage's script starts.
        const std::string path = UnixPath(exe);
        if (path.empty() || !WriteFileAtomically(DataFolder() / "install.txt", path + "\n"))
            LOGW("AppImage: can't record the install {}", Utf8(exe));
    }

    std::string Ask(std::string_view command, std::chrono::milliseconds timeout)
    {
        if (!Active())
            return {};
        // One question at a time, each with its own number, so an old answer isn't taken
        // for this one's.
        static std::mutex mutex;
        static uint64_t next = GetTickCount64() * 1000;
        const std::scoped_lock lock(mutex);
        const std::string id = std::to_string(++next);
        const fs::path request = DataFolder() / "host-request";
        const fs::path reply = DataFolder() / "host-reply";
        if (!WriteFileAtomically(request, id + " " + std::string(command) + "\n"))
        {
            LOGW("AppImage: can't write {}", Utf8(request));
            return {};
        }
        const auto end = std::chrono::steady_clock::now() + timeout;
        do
        {
            Sleep(50);
            std::ifstream in(reply, std::ios::binary);
            std::string replyId, answer;
            if (in >> replyId >> answer && replyId == id)
                return answer;
        } while (std::chrono::steady_clock::now() < end);
        LOGW("AppImage: no answer to {} (is the AppImage still running?)", command);
        std::error_code ec;
        fs::remove(request, ec);
        return {};
    }
}
