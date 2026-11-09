// ue4probe — derive the object-array layout from a live UE4 process instead of
// assuming it.
//
// Strategy: find a real UObject first (vtable inside the module's text, name
// and class both resolving through the already-located FNamePool), read the
// InternalIndex it reports, then search the module's writable memory for a
// slot that, treated as a chunked array, yields that exact object at that
// exact index. Whatever (itemSize, chunkSize) combination makes that work IS
// the layout — nothing is hardcoded.
//
// Usage: ue4probe <pid> --module <substr> [--objects N]

#include "../common/ue4core.h"

using namespace ue4;

struct Found {
    uintptr_t addr = 0;
    uint32_t  index = 0;
    std::string name, cls;
};

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <pid> --module <substr> [--objects N]\n", argv[0]); return 2; }
    Reader R{ (pid_t)atoi(argv[1]) };
    std::string moduleFilter, className_;
    int wantObjects = 24;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) moduleFilter = argv[++i];
        else if (!strcmp(argv[i], "--objects") && i + 1 < argc) wantObjects = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--class") && i + 1 < argc) className_ = argv[++i];
    }

    auto regions = readMaps(R.pid);
    if (regions.empty()) { fprintf(stderr, "no maps\n"); return 1; }

    // Executable range of the module — a UObject's vtable must point in here.
    uintptr_t textLo = 0, textHi = 0;
    for (const auto& r : regions) {
        if (r.path.find(moduleFilter) == std::string::npos) continue;
        if (r.writable) continue;
        // r-x mapping is the one we want; approximate by taking the widest
        // non-writable span of the module.
        if (!textLo || r.start < textLo) textLo = r.start;
        if (r.end > textHi) textHi = r.end;
    }
    printf("module text span: 0x%zx - 0x%zx\n", (size_t)textLo, (size_t)textHi);

    auto modScan = moduleWritableRegions(regions, moduleFilter);
    printf("module writable regions: %zu\n", modScan.size());

    PoolLayout P;
    if (!findNamePool(R, regions, modScan, P, false)) { printf("GNames NOT FOUND\n"); return 1; }
    printf("GNames: 0x%zx  name[0]=\"%s\"\n", (size_t)P.base, decodeName(R, P, 0).c_str());

    auto inText = [&](uintptr_t p) { return p >= textLo && p < textHi; };

    // ---- class layout mode -------------------------------------------------
    // Find a named UClass, then ask which of its slots actually holds a
    // property chain, by testing every 8-byte slot in the UStruct region for a
    // link whose class name ends in "Property" — both as an FField (class ptr
    // at +0x08, FName at +0x00 of FFieldClass) and as a UObject UField.
    if (!className_.empty()) {
        ObjLayout O;
        if (!findObjectArray(R, regions, modScan, P, O, true)) {
            printf("object array not found\n"); return 1;
        }
        printf("objects=%u itemSize=0x%zx chunk=%zu\n\n",
               O.numElements, O.itemSize, O.objectsPerChunk);

        uintptr_t target = 0;
        for (uint32_t i = 0; i < O.numElements; ++i) {
            uintptr_t obj = objectAt(R, O, i);
            if (!obj) continue;
            if (objectName(R, P, O, obj) != className_) continue;
            std::string c = className(R, P, O, obj);
            if (c != "Class" && c != "BlueprintGeneratedClass" && c != "ScriptStruct") continue;
            target = obj;
            printf("class \"%s\" (%s) at 0x%zx\n", className_.c_str(), c.c_str(), (size_t)obj);
            break;
        }
        if (!target) { printf("class not found\n"); return 1; }

        printf("\nraw UStruct slots:\n");
        for (size_t off = 0x28; off <= 0x70; off += 8) {
            uintptr_t v = R.ptr(target + off);
            uint32_t lo = R.get<uint32_t>(target + off);
            uint32_t hi = R.get<uint32_t>(target + off + 4);
            printf("  +0x%02zx = 0x%016zx   (u32: %-10u %-10u)%s\n", off, (size_t)v, lo, hi,
                   mapped(regions, v) ? "  <- mapped" : "");
        }

        printf("\ntesting each slot as a property chain head:\n");
        for (size_t off = 0x28; off <= 0x70; off += 8) {
            uintptr_t head = R.ptr(target + off);
            if (!mapped(regions, head)) continue;

            // (a) FField: ClassPrivate at +0x08 -> FFieldClass, FName at +0x00
            for (size_t fcOff : { (size_t)0x08, (size_t)0x10 }) {
                uintptr_t fc = R.ptr(head + fcOff);
                if (!mapped(regions, fc)) continue;
                std::string n = decodeName(R, P, R.get<uint32_t>(fc));
                if (n.size() > 8 && n.compare(n.size() - 8, 8, "Property") == 0) {
                    printf("  FField  @+0x%02zx: classPtr@+0x%02zx -> \"%s\"", off, fcOff, n.c_str());
                    for (size_t nameOff : { (size_t)0x20, (size_t)0x28, (size_t)0x30 }) {
                        std::string pn = decodeName(R, P, R.get<uint32_t>(head + nameOff));
                        if (!pn.empty()) printf("   name@+0x%02zx=\"%s\"", nameOff, pn.c_str());
                    }
                    printf("\n");
                }
            }
            // (b) UObject UField: ClassPrivate at +0x10, FName at +0x18
            uintptr_t uc = R.ptr(head + 0x10);
            if (mapped(regions, uc)) {
                std::string n = decodeName(R, P, R.get<uint32_t>(uc + 0x18));
                if (n.size() > 8 && n.compare(n.size() - 8, 8, "Property") == 0)
                    printf("  UField  @+0x%02zx: class \"%s\" name \"%s\"\n", off, n.c_str(),
                           decodeName(R, P, R.get<uint32_t>(head + 0x18)).c_str());
            }
        }
        return 0;
    }

    // ---- 1. hunt for real UObjects anywhere writable -----------------------
    std::vector<Found> found;
    auto allScan = scanCandidates(regions, false);
    for (const auto& reg : allScan) {
        if ((int)found.size() >= wantObjects) break;
        std::vector<uint8_t> buf(reg.size());
        if (!R.read(reg.start, buf.data(), buf.size())) continue;

        for (size_t off = 0; off + 0x28 <= buf.size() && (int)found.size() < wantObjects; off += 8) {
            uintptr_t vt;  memcpy(&vt, buf.data() + off, 8);
            if (!inText(untag(vt))) continue;

            uintptr_t obj = reg.start + off;
            uint32_t idx;  memcpy(&idx, buf.data() + off + 0x0C, 4);
            if (idx == 0 || idx > 10000000) continue;

            uintptr_t cls; memcpy(&cls, buf.data() + off + 0x10, 8);
            cls = untag(cls);
            if (!mapped(regions, cls)) continue;
            if (!inText(R.ptr(cls))) continue;              // class is a UObject too

            uint32_t nameId; memcpy(&nameId, buf.data() + off + 0x18, 4);
            std::string nm = decodeName(R, P, nameId);
            if (nm.empty()) continue;
            std::string cn = decodeName(R, P, R.get<uint32_t>(cls + 0x18));
            if (cn.empty()) continue;

            found.push_back({ obj, idx, nm, cn });
        }
    }

    printf("\nreal UObjects located: %zu\n", found.size());
    for (size_t i = 0; i < found.size() && i < 10; ++i)
        printf("  idx=%-8u 0x%zx  %-28s class=%s\n", found[i].index,
               (size_t)found[i].addr, found[i].name.c_str(), found[i].cls.c_str());
    if (found.empty()) { printf("cannot proceed without a known object\n"); return 1; }

    // ---- 2. find the slot + layout that reproduces them --------------------
    const size_t itemSizes[]  = { 0x18, 0x20, 0x10, 0x14, 0x28 };
    const size_t chunkSizes[] = { 65536, 16384, 32768, 8192 };

    printf("\nsearching module writable memory for the array...\n");
    size_t hits = 0;
    for (const auto& reg : modScan) {
        std::vector<uint8_t> buf(reg.size());
        if (!R.read(reg.start, buf.data(), buf.size())) continue;

        for (size_t off = 0; off + 8 <= buf.size(); off += 8) {
            uintptr_t chunks; memcpy(&chunks, buf.data() + off, 8);
            chunks = untag(chunks);
            if (!mapped(regions, chunks)) continue;

            for (size_t is : itemSizes) {
                for (size_t cs : chunkSizes) {
                    // require the first 3 known objects to all land correctly
                    size_t ok = 0, tried = 0;
                    for (const auto& f : found) {
                        if (tried >= 3) break;
                        tried++;
                        uintptr_t chunk = R.ptr(chunks + (f.index / cs) * 8);
                        if (!chunk) break;
                        if (R.ptr(chunk + (f.index % cs) * is) == f.addr) ok++;
                    }
                    if (tried && ok == tried) {
                        uintptr_t tu = reg.start + off;
                        printf("  HIT  slot 0x%zx (module+0x%zx)  itemSize=0x%zx chunkSize=%zu\n",
                               (size_t)tu, (size_t)(tu - textLo), is, cs);
                        for (size_t probe = 0x08; probe <= 0x24; probe += 4)
                            printf("        +0x%02zx = %u\n", probe, R.get<uint32_t>(tu + probe));
                        if (++hits >= 4) return 0;
                    }
                }
            }
        }
    }
    if (!hits) printf("  no slot in module writable memory reproduces those objects\n");
    return 0;
}
