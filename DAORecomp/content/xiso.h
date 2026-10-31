#pragma once
// Reading files out of an Xbox 360 disc image (XDVDFS), as scripts/xiso.py does: setup
// shows the game's menu art from the player's disc image before it's installed, and
// DLC packages are installed straight from the Ultimate Edition's second disc.

namespace dao::content
{
    // A file on a disc image: where its bytes are in the image (files are stored in
    // one piece).
    struct IsoFile
    {
        std::string path;   // '/'-separated, as on the disc (the disc's to choose)
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    // Copies the file at `path` ('/'-separated, any case, e.g.
    // "packages/core/data/guiexport.erf") out of the disc image to `out`, if it's at
    // most maxSize bytes. False, with `error`, if it can't.
    bool ExtractIsoFile(const std::filesystem::path& iso, std::string_view path, const std::filesystem::path& out,
        uint64_t maxSize, std::string& error);

    // Whether the disc image has a file at `path` (as ExtractIsoFile takes it).
    bool IsoHasFile(const std::filesystem::path& iso, std::string_view path);

    // The disc image's files, up to maxDepth folders deep and maxFiles entries looked
    // at. False, with `error`, if it isn't a disc image.
    bool ListIsoFiles(const std::filesystem::path& iso, int maxDepth, size_t maxFiles, std::vector<IsoFile>& files,
        std::string& error);
}
