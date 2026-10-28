#pragma once
// Which release the launcher offers as an update: GitHub's list of releases read,
// and the newest one newer than this build on its update channel (see
// UpdateChannel in settings.h).

#include <climits>

#include "settings.h"

namespace dao::launcher
{
    struct Release
    {
        std::string tag;
        std::string name;
        // Its notes (Markdown), shortened if very long.
        std::string notes;
        std::string pageUrl;
        std::string installerUrl;
        uint64_t installerSize = 0;
        std::array<uint8_t, 32> installerSha256 = {};
    };

    // The repository, under its name and the one it had before (which GitHub
    // redirects to it, and the only one launchers up to v0.10.0-dev.5 accept).
    // Release pages are only opened under its releases/ (see IsReleasePage) and
    // installers only taken from its releases/download/.
    inline constexpr std::array<std::string_view, 2> kRepositories = {
        "https://github.com/96dot/Dragon-Age-Origins-Recomp", "https://github.com/96dot/DAORecomp" };
    // The exe players download (setup, scripts/build_setup_exe.py): it updates
    // the install it's given. Releases also carry it as DAORecomp-Installer.exe,
    // the name launchers up to v0.5.1 look for.
    inline constexpr std::string_view kInstallerName = "DAORecomp.exe";

    // A version tag as numbers {X, Y, Z, N}: "vX.Y.Z" (a release, N is
    // kNotDevRelease) or "vX.Y.Z-dev.N" (dev release N on the way to vX.Y.Z,
    // so before it); in that order, compared as an array.
    using VersionNumber = std::array<int, 4>;
    inline constexpr int kNotDevRelease = INT_MAX;
    // Nothing for any other tag (such as "dev", or a "-dirty" build from source).
    // (Settings' DefaultUpdateChannel is defined in release_list.cpp, with this.)
    std::optional<VersionNumber> ParseVersion(std::string_view tag);
    inline bool IsDevRelease(const VersionNumber& version) { return version[3] != kNotDevRelease; }

    // Whether `url` is a page under one of kRepositories' releases/.
    bool IsReleasePage(std::string_view url);

    // Sets `update` to the newest release in GitHub's release list (JSON) newer
    // than `current` with a verifiable installer, on `channel` (Release: not dev
    // releases), or nothing if there is none; false if the list can't be read.
    bool FindUpdate(std::string_view json, std::string_view current, UpdateChannel channel,
        std::optional<Release>& update);
}
