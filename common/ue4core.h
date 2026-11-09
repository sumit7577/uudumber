// ue4core — shared target access and structure discovery for UE4 processes.
//
// Everything here works against a remote process through process_vm_readv and
// derives layout from structural invariants rather than hardcoded profiles.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#if defined(__linux__)
#include <sys/uio.h>
#endif

namespace ue4 {

// Android 11+ tags heap pointers in the top byte (TBI). Dereferencing a tagged
// pointer works because the kernel ignores the tag, but comparing one against
// /proc/pid/maps does not — it matches no mapping. Mask before any range test.
static inline uintptr_t untag(uintptr_t p) { return p & 0x00FFFFFFFFFFFFFFull; }

// pid 0 means "this process": reads become memcpy. That lets the emitter be
// exercised on a host with no device, against a target that builds the
// structures in its own address space.
struct Reader {
    pid_t pid = 0;

    bool read(uintptr_t addr, void* out, size_t n) const {
        if (pid == 0) {
            if (!addr) return false;
            memcpy(out, (const void*)addr, n);
            return true;
        }
#if defined(__linux__)
        iovec local{ out, n };
        iovec remote{ (void*)addr, n };
        return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)n;
#else
        return false;
#endif
    }

    template <typename T> T get(uintptr_t addr, bool* ok = nullptr) const {
        T v{};
        bool r = read(addr, &v, sizeof(T));
        if (ok) *ok = r;
        return r ? v : T{};
    }

    uintptr_t ptr(uintptr_t addr, bool* ok = nullptr) const {
        return untag((uintptr_t)get<uint64_t>(addr, ok));
    }
};

struct Region {
    uintptr_t start = 0, end = 0;
    bool      writable = false;
    std::string path;
    size_t size() const { return end - start; }
};

inline std::vector<Region> readMaps(pid_t pid) {
    std::vector<Region> out;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return out;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s, e;
        char perms[8] = {0}, name[512] = {0};
        int n = sscanf(line, "%zx-%zx %7s %*s %*s %*s %511[^\n]", &s, &e, perms, name);
        if (n < 3) continue;
        std::string p = (n == 4) ? name : "";
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        out.push_back({ s, e, perms[1] == 'w', p });
    }
    fclose(f);
    return out;
}

inline bool mapped(const std::vector<Region>& rs, uintptr_t addr) {
    if (addr < 0x1000 || (addr & 0x7) != 0) return false;
    for (const auto& r : rs)
        if (addr >= r.start && addr < r.end) return true;
    return false;
}

// Writable regions worth scanning, smallest first (.bss-like before heap).
inline std::vector<Region> scanCandidates(const std::vector<Region>& regions, bool all) {
    std::vector<Region> scan;
    for (const auto& r : regions) {
        if (!r.writable) continue;
        if (!all && r.size() > (64u << 20)) continue;
        if (r.path == "[vvar]" || r.path == "[vsyscall]") continue;
        scan.push_back(r);
    }
    std::sort(scan.begin(), scan.end(),
              [](const Region& a, const Region& b) { return a.size() < b.size(); });
    return scan;
}

// Writable regions belonging to a module: its own file-backed rw mappings,
// plus anonymous rw regions physically contiguous with them, which is where
// .bss lands. On a real game this is a handful of regions instead of ~1900.
inline std::vector<Region> moduleWritableRegions(const std::vector<Region>& regions,
                                                 const std::string& filter) {
    std::vector<Region> sorted = regions;
    std::sort(sorted.begin(), sorted.end(),
              [](const Region& a, const Region& b) { return a.start < b.start; });

    std::vector<Region> out;
    bool inModule = false;
    uintptr_t prevEnd = 0;
    for (const auto& r : sorted) {
        bool isModule = !filter.empty() && r.path.find(filter) != std::string::npos;
        if (isModule) {
            inModule = true;
        } else if (inModule) {
            // Keep walking while anonymous and contiguous. Android names
            // anonymous maps, so .bss shows up as "[anon:.bss]" rather than an
            // empty path — testing for empty alone misses the region that
            // actually holds the engine globals.
            bool anon = r.path.empty() || r.path.compare(0, 6, "[anon:") == 0;
            if (!(anon && r.start == prevEnd)) inModule = false;
        }
        if (inModule && r.writable) out.push_back(r);
        prevEnd = r.end;
    }
    return out;
}

inline uintptr_t moduleBase(const std::vector<Region>& regions, const std::string& filter,
                            std::string* pathOut = nullptr) {
    uintptr_t base = 0;
    for (const auto& r : regions) {
        if (r.path.empty() || r.path[0] != '/') continue;
        if (!filter.empty() && r.path.find(filter) == std::string::npos) continue;
        if (filter.empty() && r.path.find("/data/local/tmp") == std::string::npos
                           && r.path.find("libUE4") == std::string::npos) continue;
        if (!base || r.start < base) { base = r.start; if (pathOut) *pathOut = r.path; }
    }
    return base;
}

// ---------------------------------------------------------------------------
// FNamePool (4.23+)
// ---------------------------------------------------------------------------
struct PoolLayout {
    uintptr_t base = 0;
    size_t    toBlocks = 0x10;
    size_t    stride = 2;
    size_t    entryToString = 2;
    size_t    lenBit = 6;
    size_t    toCurrentBlock = 0x08;
    size_t    toCurrentByteCursor = 0x0C;
};

// Decode one FName id. The header is read UNSIGNED on purpose: a signed read
// makes any name of length >= 512 shift negative and vanish.
inline std::string decodeName(const Reader& R, const PoolLayout& P, uint32_t id,
                              bool allowWide = true) {
    uint32_t block  = id >> 16;
    uint16_t offset = (uint16_t)(id & 0xFFFF);

    bool ok = false;
    uintptr_t blockPtr = R.ptr(P.base + P.toBlocks + block * 8, &ok);
    if (!ok || !blockPtr) return "";

    uintptr_t entry = blockPtr + (size_t)offset * P.stride;
    uint16_t header = R.get<uint16_t>(entry, &ok);
    if (!ok) return "";
    uint32_t len = header >> P.lenBit;
    bool wide = (header & 1) != 0;
    if (len == 0 || len > 1024) return "";

    if (wide) {
        if (!allowWide) return "";
        // UE stores wide names as UTF-16, two bytes per char — NOT wchar_t,
        // which is 4 bytes on Android.
        std::vector<uint16_t> u16(len);
        if (!R.read(entry + P.entryToString, u16.data(), len * 2)) return "";
        std::string s;
        for (uint16_t c : u16) s += (c < 0x80) ? (char)c : '?';
        return s;
    }

    std::vector<char> buf(len + 1, 0);
    if (!R.read(entry + P.entryToString, buf.data(), len)) return "";
    for (uint32_t i = 0; i < len; ++i)
        if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7E) return "";
    return std::string(buf.data(), len);
}

inline bool findNamePool(const Reader& R, const std::vector<Region>& regions,
                         const std::vector<Region>& scan, PoolLayout& out, bool verbose) {
    for (const auto& reg : scan) {
        std::vector<uint8_t> buf(reg.size());
        if (!R.read(reg.start, buf.data(), buf.size())) continue;

        for (size_t off = 0; off + 8 <= buf.size(); off += 8) {
            uintptr_t blockPtr;
            memcpy(&blockPtr, buf.data() + off, 8);
            blockPtr = untag(blockPtr);
            if (!mapped(regions, blockPtr)) continue;

            uint8_t head[8];
            if (!R.read(blockPtr, head, sizeof(head))) continue;
            uint16_t header;
            memcpy(&header, head, 2);
            if ((header >> 6) != 4 || (header & 1)) continue;
            if (memcmp(head + 2, "None", 4) != 0) continue;

            PoolLayout p;
            p.base = reg.start + off - p.toBlocks;

            uint32_t curBlock  = R.get<uint32_t>(p.base + p.toCurrentBlock);
            uint32_t curCursor = R.get<uint32_t>(p.base + p.toCurrentByteCursor);
            if (curBlock > 8192 || curCursor > 2 * 65536) continue;
            if (decodeName(R, p, 3).empty()) continue;

            if (verbose)
                printf("  [pool] Blocks[0] slot at 0x%zx -> block 0x%zx, blocks=%u cursor=0x%x\n",
                       (size_t)(reg.start + off), (size_t)blockPtr, curBlock + 1, curCursor);
            out = p;
            return true;
        }
    }
    return false;
}

// Walk every entry in every block, yielding (id, name). This is the correct
// version of UE4Dumper's DumpBlocks423: the id offset advances by the whole
// aligned entry size, not by StrLength/stride.
template <typename Fn>
void forEachName(const Reader& R, const PoolLayout& P, Fn&& fn) {
    uint32_t blocks = R.get<uint32_t>(P.base + P.toCurrentBlock);
    uint32_t cursor = R.get<uint32_t>(P.base + P.toCurrentByteCursor);
    const size_t blockSize = P.stride * 65536;

    for (uint32_t b = 0; b <= blocks; ++b) {
        uintptr_t blockPtr = R.ptr(P.base + P.toBlocks + b * 8);
        if (!blockPtr) continue;
        size_t limit = (b == blocks) ? cursor : blockSize;

        size_t at = 0;
        while (at + P.entryToString <= limit) {
            uint16_t header = R.get<uint16_t>(blockPtr + at);
            uint32_t len = header >> P.lenBit;
            bool wide = (header & 1) != 0;
            if (len == 0) break;                       // null terminator entry

            uint32_t id = (b << 16) | (uint32_t)(at / P.stride);
            fn(id, decodeName(R, P, id));

            size_t bytes = P.entryToString + (size_t)len * (wide ? 2 : 1);
            at += (bytes + P.stride - 1) & ~(P.stride - 1);
        }
    }
}

// ---------------------------------------------------------------------------
// FUObjectArray
// ---------------------------------------------------------------------------
struct ObjLayout {
    uintptr_t fuObjectArray = 0;
    size_t    toTUObjectArray = 0x10;
    size_t    toNumElements = 0x14;
    size_t    itemSize = 0x18;
    size_t    objectsPerChunk = 0x10000;
    size_t    objToInternalIndex = 0x0C;
    size_t    objToClass = 0x10;
    size_t    objToName  = 0x18;
    size_t    objToOuter = 0x20;
    size_t    fieldToNext = 0x28;
    size_t    structToSuper = 0x30;
    size_t    structToChildren = 0x38;
    // Legacy: properties are UObjects chained on UStruct::Children.
    size_t    propToArrayDim = 0x30;
    size_t    propToElementSize = 0x34;
    size_t    propToOffset = 0x44;
    // UE 4.25+ / UE5: properties moved out of the UObject system into FField,
    // chained on UStruct::ChildProperties. Children then holds only UFunctions,
    // so a dumper that walks Children alone finds classes with no members.
    size_t    structToChildProperties = 0x40;
    size_t    fFieldToClass = 0x08;   // FFieldClass*, whose FName sits at +0x00
    size_t    fFieldToNext  = 0x20;
    size_t    fFieldToName  = 0x28;
    size_t    fPropToArrayDim = 0x38;
    size_t    fPropToElementSize = 0x3C;
    size_t    fPropToOffset = 0x4C;
    uint32_t  numElements = 0;
};

inline uintptr_t objectAt(const Reader& R, const ObjLayout& O, uint32_t index) {
    uintptr_t tu = O.fuObjectArray + O.toTUObjectArray;
    uintptr_t chunks = R.ptr(tu);
    if (!chunks) return 0;
    uintptr_t chunk = R.ptr(chunks + (index / O.objectsPerChunk) * 8);
    if (!chunk) return 0;
    return R.ptr(chunk + (index % O.objectsPerChunk) * O.itemSize);
}

// UObject::InternalIndex equals the object's own slot in the array. That is
// the strongest invariant available here and the one that separates a real
// FUObjectArray from a pointer that merely looks like one.
//
// What a real build adds over a synthetic one: HOLES. Destroyed objects leave
// null slots, and GC leaves whole ranges empty, so requiring every sampled
// slot to resolve rejects the true array. Score instead — ignore holes, and
// judge on whether the objects that DO resolve self-report the index they were
// looked up at.
struct ArrayScore {
    uint32_t sampled = 0, live = 0, named = 0, indexMatch = 0;
};

inline ArrayScore scoreArray(const Reader& R, const std::vector<Region>& regions,
                             const PoolLayout& P, const ObjLayout& O,
                             uint32_t n, uint32_t samples) {
    ArrayScore sc;
    for (uint32_t k = 0; k < samples; ++k) {
        uint32_t i = (uint32_t)((uint64_t)n * k / samples);
        if (i >= n) break;
        sc.sampled++;

        uintptr_t obj = objectAt(R, O, i);
        if (!mapped(regions, obj)) continue;              // hole — not a failure
        sc.live++;

        if (decodeName(R, P, R.get<uint32_t>(obj + O.objToName)).empty()) continue;
        uintptr_t cls = R.ptr(obj + O.objToClass);
        if (!mapped(regions, cls)) continue;
        if (decodeName(R, P, R.get<uint32_t>(cls + O.objToName)).empty()) continue;
        sc.named++;

        if (R.get<uint32_t>(obj + O.objToInternalIndex) == i) sc.indexMatch++;
    }
    return sc;
}

// Cheap pre-filter. A full score costs hundreds of reads and there can be
// millions of candidates, so first ask whether ANY of three spread-out slots
// yields an object that self-reports its own index. Garbage almost never does.
inline bool quickArrayCheck(const Reader& R, const std::vector<Region>& regions,
                            const PoolLayout& P, const ObjLayout& O, uint32_t n) {
    const uint32_t probes[3] = { 1u, n / 2u, n - 1u };
    for (uint32_t i : probes) {
        if (i >= n) continue;
        uintptr_t obj = objectAt(R, O, i);
        if (!mapped(regions, obj)) continue;
        if (R.get<uint32_t>(obj + O.objToInternalIndex) == i) return true;
    }
    return false;
}

inline bool findObjectArray(const Reader& R, const std::vector<Region>& regions,
                            const std::vector<Region>& scan, const PoolLayout& P,
                            ObjLayout& out, bool verbose) {
    // Exclude only the pool's used Blocks[] slots — a run of heap pointers and
    // the richest source of false positives. Excluding the whole 8192-entry
    // array would also exclude GUObjectArray, which on a real build shares the
    // same .bss within 64 KB of the pool.
    const uint32_t usedBlocks = R.get<uint32_t>(P.base + P.toCurrentBlock) + 1;
    const uintptr_t poolLo = P.base + P.toBlocks;
    const uintptr_t poolHi = poolLo + (uintptr_t)usedBlocks * 8;

    ObjLayout best;
    ArrayScore bestScore;
    bool haveBest = false;
    size_t nChunkPtr = 0, nCount = 0, nQuick = 0, nScored = 0, nRegionFail = 0;
    std::vector<std::pair<ArrayScore, ObjLayout>> nearMiss;

    for (const auto& reg : scan) {
        std::vector<uint8_t> buf(reg.size());
        if (!R.read(reg.start, buf.data(), buf.size())) { nRegionFail++; continue; }

        for (size_t off = 0; off + 8 <= buf.size(); off += 8) {
            uintptr_t tu = reg.start + off;
            if (tu >= poolLo && tu < poolHi) continue;

            uintptr_t chunks;
            memcpy(&chunks, buf.data() + off, 8);
            chunks = untag(chunks);
            if (!mapped(regions, chunks)) continue;

            uintptr_t chunk0 = R.ptr(chunks);
            if (!mapped(regions, chunk0)) continue;
            nChunkPtr++;

            ObjLayout O;
            O.fuObjectArray = tu - O.toTUObjectArray;

            // FUObjectItem size is NOT fixed. Stock 4.27 is 0x18 (Object,
            // Flags, ClusterRootIndex, SerialNumber padded to 8), but builds
            // that compile in the GC tracking or stat fields make it 0x20.
            // Measured 0x20 on a real packaged 4.27 game, so probe it rather
            // than assume: a wrong item size makes every index resolve to the
            // wrong object and the array looks like garbage.
            static const size_t kItemSizes[] = { 0x18, 0x20, 0x10, 0x28 };

            for (size_t probe = 0x08; probe <= 0x20; probe += 4) {
                uint32_t n = R.get<uint32_t>(tu + probe);
                if (n < 16 || n > 10000000) continue;
                nCount++;

                O.toNumElements = probe;
                O.numElements = n;

                bool quick = false;
                for (size_t is : kItemSizes) {
                    O.itemSize = is;
                    if (quickArrayCheck(R, regions, P, O, n)) { quick = true; break; }
                }
                if (!quick) continue;
                nQuick++;

                // MaxElements / MaxChunks gives the real chunk size directly;
                // sampling cannot, because a small array puts every index in
                // chunk 0 and then every chunk size looks identical.
                uint32_t maxElems = R.get<uint32_t>(tu + 0x10);
                uint32_t maxChunks = R.get<uint32_t>(tu + 0x18);
                if (maxChunks > 0 && maxChunks <= 4096 && maxElems >= n) {
                    uint32_t cs = maxElems / maxChunks;
                    if (cs == 8192 || cs == 16384 || cs == 32768 || cs == 65536)
                        O.objectsPerChunk = cs;
                }

                ArrayScore sc = scoreArray(R, regions, P, O, n, 64);
                if (nearMiss.size() < 4096) nearMiss.push_back({sc, O});
                if (sc.named < 8) continue;
                if (sc.indexMatch * 10 < sc.named * 9) continue;
                nScored++;

                // Prefer the candidate with the most self-consistent objects.
                if (!haveBest || sc.indexMatch > bestScore.indexMatch) {
                    best = O; bestScore = sc; haveBest = true;
                }
            }
        }
    }

    if (verbose) {
        printf("  [objs] chunkPtr=%zu count=%zu quick=%zu scored=%zu "
               "(unreadable regions: %zu)\n",
               nChunkPtr, nCount, nQuick, nScored, nRegionFail);
        std::sort(nearMiss.begin(), nearMiss.end(),
                  [](const std::pair<ArrayScore, ObjLayout>& a,
                     const std::pair<ArrayScore, ObjLayout>& b) {
                      return a.first.indexMatch > b.first.indexMatch;
                  });
        for (size_t i = 0; i < nearMiss.size() && i < 5; ++i) {
            const ArrayScore& sc = nearMiss[i].first;
            const ObjLayout& o = nearMiss[i].second;
            printf("  [objs] cand @0x%zx n=%-9u num@+0x%02zx  sampled=%u live=%u "
                   "named=%u idx=%u\n",
                   (size_t)(o.fuObjectArray + o.toTUObjectArray), o.numElements,
                   o.toNumElements, sc.sampled, sc.live, sc.named, sc.indexMatch);
        }
    }

    if (!haveBest) return false;
    if (verbose)
        printf("  [objs] TUObjectArray 0x%zx, NumElements@+0x%zx = %u "
               "itemSize=0x%zx chunk=%zu (sampled %u: live %u, named %u, index-match %u)\n",
               (size_t)(best.fuObjectArray + best.toTUObjectArray),
               best.toNumElements, best.numElements,
               best.itemSize, best.objectsPerChunk,
               bestScore.sampled, bestScore.live, bestScore.named,
               bestScore.indexMatch);
    out = best;
    return true;
}

inline std::string objectName(const Reader& R, const PoolLayout& P, const ObjLayout& O,
                              uintptr_t obj) {
    if (!obj) return "";
    return decodeName(R, P, R.get<uint32_t>(obj + O.objToName));
}

inline std::string className(const Reader& R, const PoolLayout& P, const ObjLayout& O,
                             uintptr_t obj) {
    uintptr_t cls = R.ptr(obj + O.objToClass);
    return cls ? objectName(R, P, O, cls) : "";
}

// Full outer chain, e.g. "Engine.Actor" or "MyGame.BP_Hero_C".
inline std::string fullPath(const Reader& R, const PoolLayout& P, const ObjLayout& O,
                            uintptr_t obj) {
    std::vector<std::string> parts;
    uintptr_t cur = obj;
    for (int depth = 0; cur && depth < 16; ++depth) {
        std::string n = objectName(R, P, O, cur);
        if (n.empty()) break;
        parts.push_back(n);
        cur = R.ptr(cur + O.objToOuter);
    }
    std::string s;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!s.empty()) s += ".";
        s += *it;
    }
    return s;
}

// ---------------------------------------------------------------------------
// GWorld
// ---------------------------------------------------------------------------
inline bool findGWorld(const Reader& R, const std::vector<Region>& regions,
                       const std::vector<Region>& scan, const PoolLayout& P,
                       const ObjLayout& O, uintptr_t& gworld, uintptr_t& world,
                       bool verbose) {
    // Every UClass has a Class Default Object, and UWorld's is named
    // "Default__World". It has class "World" like the real thing but nothing
    // points at it, so taking the first World-classed object finds the CDO and
    // then the pointer scan turns up empty. Collect real candidates instead:
    // skip CDOs, and prefer one with a usable PersistentLevel.
    std::vector<uintptr_t> candidates;
    for (uint32_t i = 0; i < O.numElements; ++i) {
        uintptr_t obj = objectAt(R, O, i);
        if (!obj) continue;
        if (className(R, P, O, obj) != "World") continue;

        std::string nm = objectName(R, P, O, obj);
        if (nm.compare(0, 9, "Default__") == 0) continue;     // CDO

        uintptr_t level = R.ptr(obj + 0x30);                  // PersistentLevel
        if (mapped(regions, level)) candidates.insert(candidates.begin(), obj);
        else                        candidates.push_back(obj);

        if (verbose)
            printf("  [world] candidate 0x%zx \"%s\" persistentLevel=%s\n",
                   (size_t)obj, nm.c_str(), mapped(regions, level) ? "yes" : "no");
    }

    if (candidates.empty()) {
        if (verbose) printf("  [world] no non-CDO UWorld instance in the object array\n");
        return false;
    }

    for (uintptr_t cand : candidates) {
        for (const auto& reg : scan) {
            std::vector<uint8_t> buf(reg.size());
            if (!R.read(reg.start, buf.data(), buf.size())) continue;
            for (size_t off = 0; off + 8 <= buf.size(); off += 8) {
                uintptr_t v;
                memcpy(&v, buf.data() + off, 8);
                if (untag(v) != cand) continue;
                gworld = reg.start + off;
                world = cand;
                return true;
            }
        }
    }

    // Nothing holds it in the scanned range, but the instance itself is still
    // useful to report.
    world = candidates.front();
    return false;
}

} // namespace ue4
