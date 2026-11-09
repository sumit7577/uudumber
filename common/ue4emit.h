// ue4emit — turn a located FNamePool / FUObjectArray into Logs.txt,
// Names.txt, Objects.txt and a C++ SDK.
//
// Split out from the dumper so it can run against a local target (pid 0) with
// no device, which is how the emitter is regression-tested.

#pragma once

#include "ue4core.h"
#include <map>
#include <ctime>
#include <sys/stat.h>

namespace ue4 {

// Cheap guard before dereferencing a chain pointer. Real FField/UField nodes
// are heap allocations: 8-byte aligned and well above the first page.
static inline bool looksLikePointer(uintptr_t p) {
    return p >= 0x10000 && (p & 0x7) == 0;
}

struct Prop {
    std::string name, typeName;
    uint32_t offset = 0, elementSize = 0, arrayDim = 1;
    uint64_t end() const { return (uint64_t)offset + (uint64_t)elementSize * arrayDim; }
};

struct Klass {
    uintptr_t addr = 0, super = 0;
    std::string name, path;
    std::vector<Prop> props;
};

static bool isPropertyClass(const std::string& s) {
    return s.size() > 8 && s.compare(s.size() - 8, 8, "Property") == 0;
}

// Map a UProperty's class to a C++ type. Anything unrecognised falls back to
// a byte blob of the right size, which keeps member offsets correct.
static std::string cppType(const std::string& propClass, uint32_t elementSize) {
    if (propClass == "FloatProperty")     return "float";
    if (propClass == "DoubleProperty")    return "double";
    if (propClass == "IntProperty")       return "int32_t";
    if (propClass == "Int64Property")     return "int64_t";
    if (propClass == "UInt32Property")    return "uint32_t";
    if (propClass == "ByteProperty")      return "uint8_t";
    if (propClass == "BoolProperty")      return "bool";
    if (propClass == "ObjectProperty")    return "class UObject*";
    if (propClass == "ClassProperty")     return "class UClass*";
    if (propClass == "NameProperty")      return "struct FName";
    if (propClass == "StrProperty")       return "struct FString";
    if (propClass == "ArrayProperty")     return "struct TArray";
    if (propClass == "StructProperty")    return "struct FStruct";
    char buf[64];
    snprintf(buf, sizeof(buf), "uint8_t[0x%X]", elementSize);
    return buf;
}

static std::string sanitize(std::string s) {
    for (char& c : s)
        if (!isalnum((unsigned char)c) && c != '_') c = '_';
    if (!s.empty() && isdigit((unsigned char)s[0])) s.insert(s.begin(), '_');
    return s;
}


struct EmitResult {
    uint32_t names = 0, objects = 0, classes = 0;
    uint32_t legacyProps = 0;   // UProperty on Children   (<= 4.24)
    uint32_t fieldProps  = 0;   // FProperty on ChildProperties (4.25+)
};

inline EmitResult emitDump(const Reader& R, const PoolLayout& P, const ObjLayout& O,
                           uintptr_t base, const std::string& mainPath,
                           const std::string& outDir, bool haveWorld,
                           uintptr_t gworld, uintptr_t world) {
    EmitResult res;
    mkdir(outDir.c_str(), 0755);
    std::string sdkDir = outDir + "/SDK";
    mkdir(sdkDir.c_str(), 0755);

    // ---- Logs.txt ----------------------------------------------------------
    {
        FILE* f = fopen((outDir + "/Logs.txt").c_str(), "w");
        fprintf(f, "Module        : %s\n", mainPath.c_str());
        fprintf(f, "Base Address  : 0x%zx\n", (size_t)base);
        fprintf(f, "GNames        : 0x%zx   +0x%zx\n", (size_t)P.base, (size_t)(P.base - base));
        fprintf(f, "GUObjectArray : 0x%zx   +0x%zx\n", (size_t)O.fuObjectArray,
                (size_t)(O.fuObjectArray - base));
        if (haveWorld)
            fprintf(f, "GWorld        : 0x%zx   +0x%zx\n", (size_t)gworld, (size_t)(gworld - base));
        else
            fprintf(f, "GWorld        : not found\n");
        fprintf(f, "\nMeasured offsets:\n");
        fprintf(f, "  TUObjectArrayToNumElements  0x%02zx\n", O.toNumElements);
        fprintf(f, "  FUObjectItemSize            0x%02zx\n", O.itemSize);
        fprintf(f, "  FNameStride                 0x%02zx\n", P.stride);
        fprintf(f, "  FNameEntryToString          0x%02zx\n", P.entryToString);
        fprintf(f, "\nObjects       : %u\n", O.numElements);
        fclose(f);
    }

    // ---- Names.txt ---------------------------------------------------------
    // counts accumulate into res
    {
        FILE* f = fopen((outDir + "/Names.txt").c_str(), "w");
        forEachName(R, P, [&](uint32_t id, const std::string& s) {
            if (s.empty()) return;
            fprintf(f, "[0x%08x] %s\n", id, s.c_str());
            res.names++;
        });
        fclose(f);
    }

    // ---- Objects.txt + class collection ------------------------------------
    std::map<uintptr_t, Klass> classes;

    {
        FILE* f = fopen((outDir + "/Objects.txt").c_str(), "w");
        for (uint32_t i = 0; i < O.numElements; ++i) {
            uintptr_t obj = objectAt(R, O, i);
            if (!obj) continue;
            std::string name = objectName(R, P, O, obj);
            if (name.empty()) continue;
            std::string cls = className(R, P, O, obj);

            fprintf(f, "[0x%06x] %-40s %-24s 0x%zx\n", i, fullPath(R, P, O, obj).c_str(),
                    cls.c_str(), (size_t)obj);
            res.objects++;

            if (cls == "Class" || cls == "BlueprintGeneratedClass" || cls == "ScriptStruct") {
                Klass k;
                k.addr  = obj;
                k.name  = name;
                k.path  = fullPath(R, P, O, obj);
                k.super = R.ptr(obj + O.structToSuper);
                classes[obj] = k;
            }
        }
        fclose(f);
    }

    // ---- properties --------------------------------------------------------
    // Two layouts have to be handled. Up to 4.24 properties are UObjects on
    // UStruct::Children. From 4.25 they are FFields on ChildProperties, and
    // Children keeps only UFunctions. Walk both; a given class uses one.
    uint32_t legacyProps = 0, fieldProps = 0;

    for (auto& kv : classes) {
        // Legacy UProperty chain
        uintptr_t child = R.ptr(kv.first + O.structToChildren);
        for (int guard = 0; looksLikePointer(child) && guard < 4096; ++guard) {
            std::string pcls = className(R, P, O, child);
            if (isPropertyClass(pcls)) {
                Prop pr;
                pr.name        = objectName(R, P, O, child);
                pr.offset      = R.get<uint32_t>(child + O.propToOffset);
                pr.elementSize = R.get<uint32_t>(child + O.propToElementSize);
                pr.arrayDim    = R.get<uint32_t>(child + O.propToArrayDim);
                if (pr.arrayDim == 0 || pr.arrayDim > 4096) pr.arrayDim = 1;
                pr.typeName    = cppType(pcls, pr.elementSize);
                if (!pr.name.empty()) { kv.second.props.push_back(pr); legacyProps++; }
            }
            child = R.ptr(child + O.fieldToNext);
        }

        // FField chain (4.25+/UE5)
        uintptr_t f = R.ptr(kv.first + O.structToChildProperties);
        for (int guard = 0; looksLikePointer(f) && guard < 4096; ++guard) {
            uintptr_t fclass = R.ptr(f + O.fFieldToClass);
            std::string pcls = looksLikePointer(fclass)
                             ? decodeName(R, P, R.get<uint32_t>(fclass)) : "";
            if (isPropertyClass(pcls)) {
                Prop pr;
                pr.name        = decodeName(R, P, R.get<uint32_t>(f + O.fFieldToName));
                pr.offset      = R.get<uint32_t>(f + O.fPropToOffset);
                pr.elementSize = R.get<uint32_t>(f + O.fPropToElementSize);
                pr.arrayDim    = R.get<uint32_t>(f + O.fPropToArrayDim);
                if (pr.arrayDim == 0 || pr.arrayDim > 4096) pr.arrayDim = 1;
                pr.typeName    = cppType(pcls, pr.elementSize);
                if (!pr.name.empty()) { kv.second.props.push_back(pr); fieldProps++; }
            }
            f = R.ptr(f + O.fFieldToNext);
        }
        std::sort(kv.second.props.begin(), kv.second.props.end(),
                  [](const Prop& a, const Prop& b) { return a.offset < b.offset; });
    }

    // Inherited size = furthest member end across all ancestors.
    auto inheritedEnd = [&](uintptr_t cls) -> uint64_t {
        uint64_t end = 0;
        uintptr_t cur = classes.count(cls) ? classes[cls].super : 0;
        for (int d = 0; cur && d < 32; ++d) {
            auto it = classes.find(cur);
            if (it == classes.end()) break;
            for (const auto& p : it->second.props) end = std::max(end, p.end());
            cur = it->second.super;
        }
        return end;
    };

    // Actor-derived classes get the A prefix, everything else U.
    auto prefixFor = [&](uintptr_t cls) -> const char* {
        uintptr_t cur = cls;
        for (int d = 0; cur && d < 32; ++d) {
            auto it = classes.find(cur);
            if (it == classes.end()) break;
            if (it->second.name == "Actor") return "A";
            cur = it->second.super;
        }
        return "U";
    };

    // ---- SDK ---------------------------------------------------------------
    {
        FILE* f = fopen((sdkDir + "/Game_classes.hpp").c_str(), "w");
        fprintf(f, "// Generated by ue4dump. Offsets are measured, not assumed.\n");
        fprintf(f, "#pragma once\n#include <cstdint>\n\n");

        for (const auto& kv : classes) {
            const Klass& k = kv.second;
            std::string cname = std::string(prefixFor(k.addr)) + sanitize(k.name);
            std::string sname;
            if (classes.count(k.super))
                sname = std::string(prefixFor(k.super)) + sanitize(classes[k.super].name);

            uint64_t inh = inheritedEnd(k.addr);
            uint64_t size = inh;
            for (const auto& p : k.props) size = std::max(size, p.end());

            fprintf(f, "// Class %s\n", k.path.c_str());
            fprintf(f, "// Size: 0x%04llX (Inherited: 0x%04llX)\n",
                    (unsigned long long)size, (unsigned long long)inh);
            if (!sname.empty()) fprintf(f, "class %s : public %s\n{\npublic:\n", cname.c_str(), sname.c_str());
            else                fprintf(f, "class %s\n{\npublic:\n", cname.c_str());

            uint64_t cursor = inh;
            int padId = 0;
            for (const auto& p : k.props) {
                if (p.offset > cursor) {
                    char pad[64];
                    snprintf(pad, sizeof(pad), "Pad_%d[0x%llX];", padId++,
                             (unsigned long long)(p.offset - cursor));
                    fprintf(f, "    %-28s %-28s // 0x%04X(0x%04llX) MISSED\n",
                            "uint8_t", pad, (unsigned)cursor,
                            (unsigned long long)(p.offset - cursor));
                    cursor = p.offset;
                }
                std::string decl = sanitize(p.name);
                if (p.arrayDim > 1) {
                    char b[32]; snprintf(b, sizeof(b), "[%u]", p.arrayDim); decl += b;
                }
                fprintf(f, "    %-28s %-28s // 0x%04X(0x%04X)\n",
                        p.typeName.c_str(), (decl + ";").c_str(), p.offset,
                        p.elementSize * p.arrayDim);
                cursor = std::max(cursor, p.end());
            }
            fprintf(f, "};\n\n");
        }
        fclose(f);
    }


    res.classes = (uint32_t)classes.size();
    res.legacyProps = legacyProps;
    res.fieldProps = fieldProps;
    return res;
}

} // namespace ue4
