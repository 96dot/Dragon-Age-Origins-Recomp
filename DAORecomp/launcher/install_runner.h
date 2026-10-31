#pragma once
// Installing from the launcher: installs the DLC chosen (content/install.h), then runs
// the installer (the Python build pipeline in installer/, as an executable or a
// script) hidden, with --progress-json, and keeps what it reports for the Install view.

#include "mods/mod_install.h"

namespace dao::launcher
{
    struct InstallRequest
    {
        std::filesystem::path iso;          // Empty to update an install whose game files are extracted.
        // Optional: the DLC (and title update) to install first: the Ultimate Edition's
        // second disc image, a folder of packages, or a package. A title update found
        // there goes to the install's title_update/, and is built in.
        std::filesystem::path dlc;
        std::filesystem::path dest;
        bool acceptLicense = false;
        bool shortcuts = false;
    };

    class InstallRunner
    {
    public:
        enum class State
        {
            Idle,
            Running,
            Done,
            Failed,
        };

        InstallRunner() = default;
        InstallRunner(const InstallRunner&) = delete;
        InstallRunner& operator=(const InstallRunner&) = delete;
        // Stops an install under way (running it again resumes it).
        ~InstallRunner();

        // False, with `error`, if the installer can't be started.
        bool Start(const InstallRequest& request, std::string& error);
        // Stops the installer and everything it started.
        void Cancel();
        // Takes what the installer reported since the last call (call once a frame).
        void Poll();

        State GetState() const { return state_; }
        const std::string& Status() const { return status_; }
        // 0..1, or negative while the step's length is unknown.
        float Progress() const { return progress_; }
        // The installer's output, the latest kMaxLogLines lines.
        const std::deque<std::string>& Log() const { return log_; }
        const std::string& Error() const { return error_; }
        // When Done: the installed game's executable.
        const std::filesystem::path& Installed() const { return installed_; }

        static constexpr size_t kMaxLogLines = 400;

    private:
        // Starts the installer for request_; false, with `error`, if it can't be.
        bool StartInstaller(std::string& error);
        void Read();
        void Take(std::string_view line);
        void ReleaseEngine();
        // Once the DLC are installed: the installer, or the DLC's error.
        void FinishDlc();

        State state_ = State::Idle;
        std::string status_, error_;
        float progress_ = -1.0f;
        std::deque<std::string> log_;
        std::filesystem::path installed_;

        // The installer's process and its job (everything it starts is in it), and
        // the thread reading its output into pending_.
        void* job_ = nullptr;
        void* process_ = nullptr;
        void* output_ = nullptr;
        std::thread reader_;
        std::mutex mutex_;
        std::vector<std::string> pending_;
        bool ended_ = false;
        DWORD exitCode_ = 0;
        // The unpacked installer's files, open so others may only read them until it
        // has ended (they were checked before it started).
        std::vector<void*> engineLocks_;

        InstallRequest request_;
        // Installing the DLC (before the installer starts): its thread, and what it
        // reports (under mutex_) once dlcDone_.
        std::jthread dlc_;
        bool dlcDone_ = false;
        std::vector<mods::InstallResult::Line> dlcLines_;
    };

    // The installer's work folder, as installer/util.py has it (DAO_WORK_DIR, else
    // %LOCALAPPDATA%\DAORecomp-build): the build tools, the build, the installer
    // itself and setup's log.
    std::filesystem::path InstallerWorkFolder();
}
