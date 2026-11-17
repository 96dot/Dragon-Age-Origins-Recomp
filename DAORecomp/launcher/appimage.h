#pragma once
// Running from the Linux AppImage (linux/AppRun), which starts this program under
// Proton and tells it, through environment variables, what it can do on the Linux side:
//   DAO_APPIMAGE       the AppImage's Linux path (what a Steam shortcut starts)
//   DAO_APPIMAGE_DATA  its data folder, as this program sees it (Z:\home\...): where
//                      the install is recorded and requests to the Linux side go
//   DAO_APPIMAGE_ICON  a PNG of the game's icon (Linux path), for Steam
//   DAO_HOST_STEAM     the Linux Steam's folder (Z:\...), when the AppImage wasn't
//                      started by Steam (closing that Steam would end the game)

namespace dao::launcher::appimage
{
    // Whether this program was started by the AppImage.
    bool Active();

    // The AppImage's Linux path and its icon's; empty if not Active.
    std::string AppImagePath();
    std::string IconPath();

    // The Linux Steam's folder, or empty.
    std::filesystem::path HostSteam();

    // Records `exe` (the installed DAORecomp.exe) as the one the AppImage starts from
    // now on; logged if it can't.
    void RecordInstall(const std::filesystem::path& exe);

    // Asks the AppImage's script on the Linux side to do `command` (one of a fixed set:
    // steam-running, steam-shutdown, steam-end, steam-start) and returns its one-word
    // answer, or empty if none came within `timeout`.
    std::string Ask(std::string_view command, std::chrono::milliseconds timeout = std::chrono::seconds(5));
}
