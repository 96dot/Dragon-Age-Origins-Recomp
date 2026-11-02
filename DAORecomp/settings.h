#pragma once
// userdata/config.toml: user-facing settings. Written with defaults on first run;
// the launcher and the game's Graphics menu edit it through ReadSettings/SaveSettings.

namespace dao
{
    enum class Upscaler { Bilinear, Cas, Fsr };
    // Temporal upscaling and anti-aliasing of the 3D scenes (AMD FidelityFX API, Intel
    // XeSS-SR or NVIDIA DLSS, experimental; those two fall back to FSR where they can't run).
    enum class TemporalUpscaler { Off, Auto, Fsr4, Fsr3, Xess, Dlss };
    // Anti-aliasing of the 3D scenes without temporal upscaling: the game's own 2xMSAA, or
    // SMAA 1x on the scenes rendered single-sampled.
    enum class AntiAliasing { Msaa, Smaa };
    // Which releases the launcher offers as updates: Release, only releases (vX.Y.Z);
    // Dev, dev releases too (vX.Y.Z-dev.N, test builds published between releases).
    enum class UpdateChannel { Release, Dev };
    // The game's language: the ones the supported disc has text for (its voices are
    // English in each). The game reads it as the console's language.
    enum class Language { English, Italian, Spanish };
    // Dev on a dev release, Release on anything else (defined in
    // launcher/release_list.cpp, with the version tags).
    UpdateChannel DefaultUpdateChannel();
    // Settings::fov and conversationFov: the game's own field of view (60 degrees
    // vertical, 91.5 across at 16:9: 91 stands for it and leaves it as it is), and
    // the limits.
    inline constexpr int kOriginalFov = 91;
    inline constexpr int kMinFov = 80;
    inline constexpr int kMaxFov = 120;
    // The values offered (the in-game Graphics tab's FOV list too).
    inline constexpr int kFovSteps[] = { 80, 85, 91, 95, 100, 105, 110, 115, 120 };
    // Settings::anisotropicFiltering values: 0 = the game default.
    inline constexpr int kAnisotropicLevels[] = { 0, 2, 4, 8, 16 };
    // Settings::texturePackMaxSize values: 0 = any size.
    inline constexpr int kTexturePackMaxSizes[] = { 0, 1024, 2048, 4096, 8192 };
    // Settings::texturePackMemory values the launcher offers (MB): 0 = automatic. Other
    // values can be set in config.toml; the launcher shows them as they are.
    inline constexpr int kTexturePackMemories[] = { 0, 512, 1024, 2048, 3072, 4096, 6144, 8192 };
    // The frame rates offered (0: unlimited). 30 is the game's own lock in its 3D
    // scenes; above 60 is experimental.
    inline constexpr int kFrameRates[] = { 30, 60, 90, 120, 144, 165, 240, 0 };
    // Above 60 fps, or unlimited: the game's presents aren't held to vblanks.
    inline bool FrameRateAbove60(int frameRate) { return frameRate == 0 || frameRate > 60; }
    // Xenia's anisotropic_override for one of kAnisotropicLevels (-1: the game's own).
    inline int XeniaAnisotropicLevel(int level)
    {
        return level ? 1 + std::countr_zero(unsigned(level)) : -1;
    }

    // A folder under mods/, in load order (later mods win over earlier ones).
    struct ModState
    {
        std::string folder;
        bool enabled = true;
    };

    struct Settings
    {
        // Show the launcher window before starting the game.
        bool showLauncher = true;
        // Look for a newer release on GitHub when the launcher opens.
        bool checkForUpdates = true;
        UpdateChannel updateChannel = DefaultUpdateChannel();
        // Whether the player chose updateChannel (config.toml has it); until then it
        // follows the running build, and isn't written.
        bool updateChannelChosen = false;
        // The warning that not every mod works was accepted (shown before the Mods
        // screen first opens).
        bool modsWarningSeen = false;

        Language language = Language::English;

        // Internal rendering resolution multiplier (the game renders at 1280x720):
        // 2 = 2560x1440, 3 = 3840x2160.
        int resolutionScale = 1;
        // Scaling/sharpening applied when presenting to a window of another size.
        Upscaler upscaler = Upscaler::Fsr;
        // FSR: sharpness reduction in stops (0 = sharpest, 2 = softest).
        // CAS: additional sharpness (0..1).
        float sharpness = 0.2f;
        // Upscales the 3D scenes to the window size with motion vectors (the UI
        // is scaled separately); Auto picks FSR 4 where supported, else FSR 3.1.
        TemporalUpscaler temporalUpscaler = TemporalUpscaler::Auto;
        float temporalSharpness = 0.3f; // 0..1, FSR's (XeSS and DLSS don't sharpen)
        // While temporalUpscaler is Off (temporal upscaling anti-aliases the scenes itself).
        AntiAliasing antiAliasing = AntiAliasing::Msaa;
        // Frame generation (FSR 3.1): with temporal upscaling, a frame generated
        // between each two of the 3D scenes, shown half a frame apart: up to twice the
        // frames on the display (only real ones while it can't show that many), half a
        // frame more latency.
        bool frameGeneration = false;
        // Waits for the display's refresh when presenting: no tearing, and no more
        // frames shown than the display refreshes.
        bool vsync = true;
        // Low latency (AMD Radeon Anti-Lag 2 or NVIDIA Reflex): each frame starts as late
        // as it can still be on time, so the controller is read closer to when it's shown.
        // 0 off, 1 on, 2 (kMaxLowLatency) on with Reflex's boost (Anti-Lag 2 has none: as 1).
        int lowLatency = 0;
        // One of kFrameRates. The game locks its 3D scenes to 30 fps through the D3D
        // presentation interval: 60 presents them every vblank, and above 60 every
        // frame at once, paced to the rate unless unlimited (gpu/video.cpp).
        int frameRate = 60;
        // HDR output (scRGB swap chain; SDR image expanded on HDR displays).
        bool hdr = false;
        float hdrPaperWhite = 200.0f; // nits
        float hdrPeak = 1000.0f;      // nits
        // HDR quality High: the 3D scenes' HDR stays scRGB (16-bit float) from the
        // upscaler to the display, instead of PQ in 10 bits: smoother gradients, more
        // memory traffic (Standard, false, is the default). Meant for temporal
        // upscaling: without it, HDR scenes at another size than the window's are
        // stretched without the output upscaler's FSR 1 or CAS.
        bool hdrHighQuality = false;
        // Motion blur of the 3D scenes, from temporal upscaling's motion vectors (so
        // only with it): 0 off, 1 low, up to kMaxMotionBlur high.
        int motionBlur = 0;
        // Depth of field in conversations (and, it seems, cutscenes), with temporal upscaling (it
        // blurs the upscaled scene): the people nearest the camera in the middle of
        // the picture sharp, the scene behind them (and anything in front of them)
        // blurred, as a film camera's lens does.
        bool depthOfField = true;
        // The game's own depth of field in conversations (the console's, a light blur
        // behind and in front of the people speaking; the recomp's above blurs over it).
        // Off, the scene stays sharp: for the recomp's alone, or none.
        bool consoleDepthOfField = true;
        // The camera's field of view in gameplay, and in conversations and cutscenes
        // (scaling each shot's own): horizontal degrees at 16:9, kMinFov to kMaxFov
        // (gpu/camera_fov.cpp).
        int fov = kOriginalFov;
        int conversationFov = kOriginalFov;
        // Anisotropic texture filtering, one of kAnisotropicLevels.
        int anisotropicFiltering = 0;
        // Texture packs (gpu/texture_pack.h): on, the largest width or height loaded
        // (one of kTexturePackMaxSizes; larger mips are left out), and the graphics
        // memory they may take in MB (0: half the graphics card's own, at least 256 MB).
        bool texturePacks = true;
        int texturePackMaxSize = 0;
        int texturePackMemory = 0;
        // The engine's dynamic character shadows and its post-processing (frame
        // buffer effects): switches of the PC version left in the 360 engine.
        bool shadows = true;
        bool postProcessing = true;
        bool fullscreen = false;
        int windowWidth = 1280;
        int windowHeight = 720;

        // Code patches by id (patches/patches.h); missing ids use the patch default.
        std::map<std::string, bool, std::less<>> patches;
        std::vector<ModState> mods;
    };

    // The limits ClampSettings keeps.
    constexpr int kMaxResolutionScale = 7;
    // Motion blur's highest level (Settings::motionBlur); the in-game Graphics tab and the
    // launcher list one name per level.
    constexpr int kMaxMotionBlur = 2;
    // Low latency's highest level (Settings::lowLatency); the in-game Graphics tab and the
    // launcher list one name per level.
    constexpr int kMaxLowLatency = 2;
    // The last language (Settings::language); the launcher lists one name per language.
    constexpr int kMaxLanguage = int(Language::Spanish);
    // The last temporal upscaler (Settings::temporalUpscaler); the in-game Graphics tab and the
    // launcher list one name per upscaler.
    constexpr int kMaxTemporalUpscaler = int(TemporalUpscaler::Dlss);
    // The last anti-aliasing (Settings::antiAliasing); the in-game Graphics tab and the
    // launcher list one name per method.
    constexpr int kMaxAntiAliasing = int(AntiAliasing::Smaa);
    constexpr float kHdrMinNits = 80.0f;
    constexpr float kHdrMaxPaperWhite = 1000.0f;
    constexpr float kHdrMaxPeak = 10000.0f;

    // Brings every value within its limits (the HDR peak at least the paper white).
    void ClampSettings(Settings& settings);

    // Motion blur as config.toml writes it: "off", "low", "high".
    std::string_view MotionBlurName(int motionBlur);

    // Anti-aliasing as config.toml writes it: "msaa", "smaa".
    std::string_view AntiAliasingName(AntiAliasing antiAliasing);

    // Low latency as config.toml writes it: "off", "on", "boost".
    std::string_view LowLatencyName(int lowLatency);

    // The HDR quality as config.toml writes it: "high", "standard".
    std::string_view HdrQualityName(bool high);

    // The language as config.toml writes it: "english", "italian", "spanish".
    std::string_view LanguageName(Language language);

    // The update channel as config.toml writes it: "release", "dev".
    std::string_view UpdateChannelName(UpdateChannel channel);

    // A frame rate for the log: "60 fps", "unlimited".
    std::string FrameRateName(int frameRate);

    // Reads config.toml as written (no environment overrides, no side effects):
    // defaults if it's missing, nothing if it can't be parsed.
    std::optional<Settings> TryReadSettings(const std::filesystem::path& userRoot);
    // TryReadSettings, with defaults if the file can't be parsed.
    Settings ReadSettings(const std::filesystem::path& userRoot);
    // Rewrites config.toml from the settings, keeping the [xenia] section as it is.
    void SaveSettings(const std::filesystem::path& userRoot, const Settings& settings);

    // Reads the file (creating it if missing), applies environment overrides and
    // the [xenia] cvar overrides. Must run before the graphics system is created.
    const Settings& LoadSettings(const std::filesystem::path& userRoot);
    const Settings& GetSettings();
}
