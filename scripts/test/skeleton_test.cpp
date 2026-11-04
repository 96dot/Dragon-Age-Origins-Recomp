#include "stdafx.h"
#include "mods/convert/mesh.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace dao::mods;

static void Check(bool pass, const char* message)
{
    if (!pass) throw std::runtime_error(message);
}

static gff::Value Number(uint32_t number)
{
    gff::Value v;
    v.type = gff::kU32;
    v.SetU32(number);
    return v;
}

static gff::File Rig(bool pc)
{
    gff::File file;
    file.root.kind = gff::Value::Kind::Struct;
    file.root.At(6256) = Number(pc ? 5 : 3);
    gff::Value nodes;
    nodes.kind = gff::Value::Kind::List;
    auto bone = [&](const char16_t* name, uint32_t index) {
        gff::Value node;
        node.kind = gff::Value::Kind::Struct;
        node.At(6000).kind = gff::Value::Kind::String;
        node.At(6000).text = name;
        node.At(6000).text.push_back(0);
        node.At(6254) = Number(index);
        nodes.items.push_back(std::move(node));
    };
    bone(u"LeftWrist", pc ? 4 : 2);
    bone(u"LeftMiddleFinger", pc ? 3 : 0);
    bone(u"LeftMiddleFinger1", 1);
    if (pc)
    {
        bone(u"LeftRingFinger", 0);
        bone(u"LeftPinkFinger1", 2);
    }
    gff::Value mesh;
    mesh.kind = gff::Value::Kind::Struct;
    mesh.At(6255).kind = gff::Value::Kind::List;
    for (uint32_t i : {4, 0, 2, 3, 1})
        mesh.At(6255).items.push_back(Number(i));
    nodes.items.push_back(std::move(mesh));
    file.root.At(6999) = std::move(nodes);
    return file;
}

static gff::File Load(const char* path)
{
    std::ifstream stream(path, std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream), {}};
    auto file = gff::Read(bytes);
    Check(bool(file), "could not read input GFF");
    return std::move(*file);
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 3)
        {
            auto pc = Load(argv[1]);
            const auto xbox = Load(argv[2]);
            const auto error = convert::ConvertSkeleton(pc, xbox);
            if (error) throw std::runtime_error(*error);
            Check(pc.root.At(6256).U32() == xbox.root.Find(6256)->U32(), "bone count differs");
            std::cout << "PASS " << argv[1] << '\n';
            return 0;
        }
        const auto xbox = Rig(false);
        auto pc = Rig(true);
        Check(!convert::ConvertSkeleton(pc, xbox), "valid skeleton rejected");
        Check(pc.root.At(6256).U32() == 3, "wrong bone count");
        auto& nodes = pc.root.At(6999).items;
        Check(nodes[0].At(6254).U32() == 2, "wrist not remapped");
        Check(nodes[1].At(6254).U32() == 0, "middle finger not remapped");
        Check(nodes[3].At(6254).U32() == ~0u && nodes[4].At(6254).U32() == ~0u, "omitted fingers indexed twice");
        const auto& palette = nodes.back().At(6255).items;
        const uint32_t expected[] = {2, 0, 1, 0, 1};
        for (size_t i = 0; i < palette.size(); ++i)
            Check(palette[i].U32() == expected[i], "palette order changed or mapping wrong");

        pc = Rig(true);
        pc.root.At(6999).items.back().At(6255).items[0].SetU32(1000);
        Check(bool(convert::ConvertSkeleton(pc, xbox)), "invalid palette accepted");
        Check(pc.root.At(6256).U32() == 5 && pc.root.At(6999).items[0].At(6254).U32() == 4,
            "failed conversion partially changed skeleton");

        pc = Rig(true);
        pc.root.At(6999).items[3].At(6000).text = u"UnknownBone";
        Check(bool(convert::ConvertSkeleton(pc, xbox)), "unknown weighted bone accepted");

        pc = Rig(true);
        auto invalid = Rig(false);
        invalid.root.At(6999).items[0].At(6254).SetU32(99);
        Check(bool(convert::ConvertSkeleton(pc, invalid)), "invalid target index accepted");

        pc = Rig(true);
        auto noSkeleton = Rig(false);
        noSkeleton.root.At(6256).SetU32(0);
        Check(bool(convert::ConvertSkeleton(pc, noSkeleton)), "missing target skeleton accepted");
        pc.root.At(6256).SetU32(0);
        Check(!convert::ConvertSkeleton(pc, noSkeleton), "rigid model rejected");

        // Stray bones on a model the game keeps rigid: nothing weighted, nothing changed.
        pc = Rig(true);
        pc.root.At(6999).items.back().At(6255).items.clear();
        Check(!convert::ConvertSkeleton(pc, noSkeleton), "unweighted bones on a rigid game model rejected");
        Check(pc.root.At(6256).U32() == 5 && pc.root.At(6999).items[0].At(6254).U32() == 4, "rigid game model's bones changed");

        pc = Rig(true);
        pc.root.At(6256).SetU32(5000);
        Check(bool(convert::ConvertSkeleton(pc, xbox)), "absurd bone count accepted");

        auto detailed = Rig(true);
        auto reduced = Rig(false);
        auto& detailedNodes = detailed.root.At(6999).items;
        auto wrist = detailedNodes[0];
        wrist.At(6254).SetU32(0);
        auto finger = detailedNodes[1];
        finger.At(6254).SetU32(1);
        wrist.At(6999).kind = gff::Value::Kind::List;
        wrist.At(6999).items.push_back(finger);
        auto mesh = detailedNodes.back();
        mesh.At(6255).items = {Number(1)};
        detailedNodes = {wrist, mesh};
        detailed.root.At(6256).SetU32(2);
        wrist.At(6999).items.clear();
        reduced.root.At(6999).items = {wrist};
        reduced.root.At(6256).SetU32(1);
        Check(bool(convert::ConvertSkeleton(detailed, reduced)), "detail conversion silently lost a weighted joint");
        Check(!convert::ConvertSkeleton(detailed, reduced, true), "distant ancestor mapping rejected");
        Check(detailed.root.At(6999).items.back().At(6255).items[0].U32() == 0, "distant finger not folded into wrist");
        std::cout << "PASS skeleton remapping, omitted fingers, reduced rigs, unknown bones, invalid inputs, bone limit, atomic failure, rigid models\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
