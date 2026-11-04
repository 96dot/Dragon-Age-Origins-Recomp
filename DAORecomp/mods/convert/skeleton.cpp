#include "stdafx.h"
#include "mods/convert/mesh.h"
#include "text.h"

#include <set>

namespace dao::mods::convert
{
    namespace
    {
        // Model fields.
        constexpr uint32_t kName = 6000, kMeshName = 6005, kBoneIndex = 6254, kBonePalette = 6255, kBoneCount = 6256;
        constexpr uint32_t kNoBone = 0xFFFFFFFF;
        // Far more than a real rig has; keeps the search for retained ancestors short on a damaged file.
        constexpr uint32_t kMaxBones = 1024;

        std::u16string Text(const gff::Value& value, uint32_t field)
        {
            const auto* name = value.Find(field);
            if (!name || name->kind != gff::Value::Kind::String)
                return {};
            auto text = name->text;
            while (!text.empty() && text.back() == 0)
                text.pop_back();
            return text;
        }

        std::u16string Name(const gff::Value& value)
        {
            return Text(value, kName);
        }

        std::string Printable(const std::u16string& name)
        {
            return WideToUtf8(std::wstring(name.begin(), name.begin() + std::min<size_t>(name.size(), 64)));
        }

        template<class Value, class Visit>
        void Walk(Value& value, const Visit& visit)
        {
            visit(value);
            for (auto& field : value.fields)
                Walk(field.value, visit);
            for (auto& item : value.items)
                Walk(item, visit);
        }

        // The console rig omits the ring and little fingers. Its own hand meshes
        // animate those fingers with the corresponding middle-finger joints.
        std::u16string ConsoleFinger(std::u16string name)
        {
            for (const auto part : { std::u16string_view(u"RingFinger"), std::u16string_view(u"PinkFinger") })
                if (const auto at = name.find(part); at != std::u16string::npos)
                {
                    name.replace(at, part.size(), u"MiddleFinger");
                    break;
                }
            return name;
        }
    }

    bool HasSkeleton(const gff::File& model)
    {
        const auto* count = model.root.Find(kBoneCount);
        return count && count->U32();
    }

    std::u16string ModelMeshName(const gff::File& model)
    {
        return Text(model.root, kMeshName);
    }

    bool HasBonesOf(const gff::File& model, const gff::File& other)
    {
        std::set<std::u16string> bones;
        Walk(model.root, [&](const gff::Value& node) {
            if (const auto* index = node.Find(kBoneIndex); index && index->U32() != kNoBone)
                bones.insert(Name(node));
        });
        bool all = true;
        Walk(other.root, [&](const gff::Value& node) {
            if (const auto* index = node.Find(kBoneIndex); index && index->U32() != kNoBone)
                all = all && bones.contains(Name(node));
        });
        return all;
    }

    std::optional<std::string> ConvertSkeleton(gff::File& model, const gff::File& gameModel, bool distant)
    {
        const auto* sourceCount = model.root.Find(kBoneCount);
        const auto* targetCount = gameModel.root.Find(kBoneCount);
        if (!sourceCount || !sourceCount->U32())
            return std::nullopt; // A rigid model.
        if (sourceCount->U32() > kMaxBones)
            return "this model has too many bones";
        if (!targetCount || !targetCount->U32())
        {
            // Stray bones on a model the game keeps rigid do no harm if nothing is weighted to them.
            bool weighted = false;
            Walk(model.root, [&](const gff::Value& node) {
                const auto* palette = node.Find(kBonePalette);
                weighted |= palette && !palette->items.empty();
            });
            if (!weighted)
                return std::nullopt;
            return "the game's copy of this model has no skeleton to match";
        }

        std::map<std::u16string, uint32_t> targetBones;
        std::map<uint32_t, std::u16string> targetNames;
        bool invalid = false;
        Walk(gameModel.root, [&](const gff::Value& node) {
            if (const auto* index = node.Find(kBoneIndex); index && index->U32() != kNoBone)
            {
                const auto name = Name(node);
                invalid |= name.empty() || index->U32() >= targetCount->U32();
                const auto [it, added] = targetBones.emplace(name, index->U32());
                invalid |= !added && it->second != index->U32();
                const auto [named, fresh] = targetNames.emplace(index->U32(), name);
                invalid |= !fresh && named->second != name;
            }
        });
        if (invalid || targetBones.empty())
            return "the game's copy of this model has unusable bone data";

        std::map<uint32_t, uint32_t> paletteMap;
        std::map<uint32_t, std::u16string> sourceNames;
        std::map<uint32_t, uint32_t> parents;
        auto findParents = [&](auto&& self, const gff::Value& value, uint32_t parent) -> void {
            if (const auto* index = value.Find(kBoneIndex); index && index->U32() != kNoBone)
            {
                parents.emplace(index->U32(), parent);
                parent = index->U32();
            }
            for (const auto& field : value.fields)
                self(self, field.value, parent);
            for (const auto& item : value.items)
                self(self, item, parent);
        };
        findParents(findParents, model.root, kNoBone);
        Walk(model.root, [&](const gff::Value& node) {
            if (const auto* index = node.Find(kBoneIndex); index && index->U32() != kNoBone)
            {
                const auto name = Name(node);
                invalid |= name.empty() || index->U32() >= sourceCount->U32();
                const auto [named, fresh] = sourceNames.emplace(index->U32(), name);
                invalid |= !fresh && named->second != name;
            }
        });
        if (invalid)
            return "this model's bone data is invalid";
        for (const auto& [index, name] : sourceNames)
        {
            auto target = targetBones.find(name);
            if (target == targetBones.end())
                target = targetBones.find(ConsoleFinger(name));
            // The console's distant rig also removes facial, finger and foot
            // joints. Use the nearest retained ancestor for those influences.
            uint32_t parent = parents.at(index);
            while (distant && target == targetBones.end() && parent != kNoBone)
            {
                const auto ancestor = sourceNames.find(parent);
                if (ancestor == sourceNames.end())
                    break;
                target = targetBones.find(ancestor->second);
                parent = parents.at(parent);
            }
            if (target != targetBones.end())
                paletteMap.emplace(index, target->second);
        }
        // Validate everything before changing it: a partial remap must never be written.
        std::string unmatched;
        Walk(model.root, [&](const gff::Value& node) {
            if (const auto* palette = node.Find(kBonePalette))
                for (const auto& bone : palette->items)
                    if (!invalid && !paletteMap.contains(bone.U32()))
                    {
                        invalid = true;
                        const auto name = sourceNames.find(bone.U32());
                        unmatched = name != sourceNames.end() ? Printable(name->second) : fmt::format("number {}", bone.U32());
                    }
        });
        if (invalid)
            return fmt::format("its bone {} has no Xbox 360 bone to match", unmatched);

        Walk(model.root, [&](gff::Value& node) {
            if (auto* index = node.Find(kBoneIndex); index && index->U32() != kNoBone)
            {
                // Bones the console rig omits remain as unindexed transform nodes; giving
                // omitted fingers their middle-finger aliases would duplicate skeleton indices.
                const auto target = targetBones.find(Name(node));
                index->SetU32(target == targetBones.end() ? kNoBone : target->second);
            }
            if (auto* palette = node.Find(kBonePalette))
                for (auto& bone : palette->items)
                    bone.SetU32(paletteMap.at(bone.U32()));
        });
        model.root.At(kBoneCount).SetU32(targetCount->U32());
        return std::nullopt;
    }
}
