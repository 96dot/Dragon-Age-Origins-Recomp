#pragma once
// PC meshes (.msh) to the Xbox 360's vertex formats, and their models' (.mmh)
// skeletons to the 360's bone numbering (skeleton.cpp).
//
// The 360 version's vertex shaders expect normals, tangents and bone weights as
// UB4N bytes (normals and tangents remapped from [-1, 1], the tangent's w the
// bitangent's handedness, no bitangent), bone indices as UB4, and the PC's
// positions and texture coordinates as they are. A UB4N/UB4 element is a 32-bit
// word with x in its low byte, so in the big-endian files its bytes are w, z, y, x.
// Halfs and floats are big-endian; the index buffer is big-endian u16s.

#include "mods/convert/gff.h"

namespace dao::mods::convert
{
    // Re-encodes a PC mesh's vertex and index buffers in place (the GFF tree, to
    // be written with the 360's templates). An error message if it can't.
    std::optional<std::string> ConvertMesh(gff::File& mesh);

    // Whether a PC mesh's vertices have bone indices.
    bool IsSkinned(const gff::File& mesh);

    // Whether a model has bones, and the name of the mesh file it uses.
    bool HasSkeleton(const gff::File& model);
    std::u16string ModelMeshName(const gff::File& model);

    // Shared character skeletons are indexed differently on PC and Xbox 360.
    // Keep the mod's per-mesh palette order (its vertex indices refer to it), but
    // resolve its entries and node indices by bone name against the game model.
    // Bones the game model lacks stop being indexed. Distant models may fold them
    // into their nearest retained parent; otherwise a weighted one is an error.
    // The model is unchanged after an error.
    std::optional<std::string> ConvertSkeleton(gff::File& model, const gff::File& gameModel,
        bool distant = false);
}
