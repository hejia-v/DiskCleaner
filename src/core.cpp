#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "core.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QFile>
#include <QSaveFile>
#include <QStringList>

namespace wt {
namespace {

struct ScanCanceled {};
void checkCanceled(const std::atomic_bool* cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) throw ScanCanceled{};
}

struct FindHandle {
    HANDLE handle;
    ~FindHandle() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
};

// Validate before adding an untrusted attribute length (which may wrap uint32_t).
bool validAttribute(const uint8_t* record, uint32_t used, uint32_t offset, uint32_t length)
{
    if (offset > used || length < 16 || length > used - offset) return false;
    return record[offset + 8] ? length >= 64 : length >= 24;
}

inline uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

std::wstring longPath(const std::wstring& p)
{
    if (p.size() >= 240 && p.rfind(L"\\\\?\\", 0) != 0) return L"\\\\?\\" + p;
    return p;
}

// ---------------------------------------------------------------- volume I/O

class Volume {
public:
    ~Volume() { close(); }

    bool open(wchar_t letter)
    {
        wchar_t path[16];
        swprintf_s(path, L"\\\\.\\%c:", letter);
        h_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            err_ = GetLastError();
            return false;
        }
        return true;
    }

    void close()
    {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    bool readAt(uint64_t off, void* buf, DWORD len)
    {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(off);
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        if (!ReadFile(h_, buf, len, &got, nullptr)) return false;
        return got == len;
    }

    bool readBoot()
    {
        uint8_t b[512] = {};
        if (!readAt(0, b, 512)) return false;
        bytesPerSector_ = rd16(b + 0x0B);
        uint8_t sectorsPerCluster = b[0x0D];
        if (bytesPerSector_ < 256 || sectorsPerCluster == 0) return false;
        clusterSize_ = uint32_t(bytesPerSector_) * sectorsPerCluster;
        mftLcn_ = rd64(b + 0x30);
        int8_t cpr = static_cast<int8_t>(b[0x40]);
        if (cpr <= 0 && (cpr == 0 || cpr < -16)) return false;
        recordSize_ = cpr > 0 ? uint32_t(cpr) * clusterSize_ : (1u << uint32_t(-cpr));
        if (recordSize_ < 512 || recordSize_ > 65536) return false;
        return true;
    }

    uint32_t bytesPerSector() const { return bytesPerSector_; }
    uint32_t clusterSize() const { return clusterSize_; }
    uint32_t recordSize() const { return recordSize_; }
    uint64_t mftLcn() const { return mftLcn_; }
    DWORD error() const { return err_; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    DWORD err_ = 0;
    uint32_t bytesPerSector_ = 512;
    uint32_t clusterSize_ = 4096;
    uint32_t recordSize_ = 1024;
    uint64_t mftLcn_ = 0;
};

// ---------------------------------------------------------------- runlists

struct Run {
    uint64_t vcn = 0;
    uint64_t lcn = 0;
    uint64_t clusters = 0;
    bool sparse = false;
};

bool parseRunList(const uint8_t* p, size_t len, std::vector<Run>& runs,
                  uint64_t& dataSize, uint64_t& allocSize)
{
    uint64_t lcn = 0;
    uint64_t vcn = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t h = p[i++];
        if (h == 0) break;
        uint32_t nLen = h & 0x0F;
        uint32_t nOff = h >> 4;
        if (nLen == 0 || nLen > 8 || nOff > 8 || i + nLen + nOff > len) return false;
        uint64_t count = 0;
        for (uint32_t k = 0; k < nLen; ++k) count |= uint64_t(p[i + k]) << (8 * k);
        i += nLen;
        int64_t delta = 0;
        if (nOff > 0) {
            uint64_t raw = 0;
            for (uint32_t k = 0; k < nOff; ++k) raw |= uint64_t(p[i + k]) << (8 * k);
            // sign-extend
            uint64_t signBit = 1ull << (nOff * 8 - 1);
            if (raw & signBit) raw |= ~((signBit << 1) - 1);
            delta = static_cast<int64_t>(raw);
            i += nOff;
        }
        Run r;
        r.vcn = vcn;
        r.clusters = count;
        if (nOff == 0) {
            r.sparse = true;
            r.lcn = 0;
        } else {
            lcn = static_cast<uint64_t>(static_cast<int64_t>(lcn) + delta);
            r.lcn = lcn;
        }
        runs.push_back(r);
        vcn += count;
    }
    (void)dataSize;
    (void)allocSize;
    return !runs.empty();
}

bool mapOffset(const std::vector<Run>& runs, uint64_t clusterSize, uint64_t off,
               uint64_t& phys, uint64_t& avail)
{
    uint64_t vcn = off / clusterSize;
    for (const Run& r : runs) {
        if (vcn >= r.vcn && vcn < r.vcn + r.clusters) {
            if (r.sparse) return false;
            uint64_t inRun = off - r.vcn * clusterSize;
            phys = r.lcn * clusterSize + inRun;
            avail = r.clusters * clusterSize - inRun;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- MFT records

struct Rec {
    uint64_t size = 0;
    uint64_t alloc = 0;
    uint64_t fnSize = 0;   // size as recorded in FILE_NAME (diagnostic)
    uint8_t hasData = 0;
    int64_t mtime = 0;
    int64_t ctime = 0;
    int64_t atime = 0;
    uint32_t recNo = 0;
    uint32_t parent = kNone;
    uint32_t attrs = 0;
    uint32_t nameOff = 0;
    uint32_t nameLen = 0;
    uint8_t ns = 2;
    uint8_t isDir = 0;
    uint8_t inUse = 0;
};

int nsPriority(uint8_t ns)
{
    switch (ns) {
    case 1: return 4;  // Win32
    case 3: return 3;  // Win32 & DOS
    case 0: return 2;  // POSIX
    case 2: return 1;  // DOS 8.3
    default: return 0;
    }
}

bool parseRecord(uint8_t* rec, uint32_t recordSize, uint32_t sectorSize,
                 std::vector<char16_t>& nameArena, Rec& out, std::atomic_bool* cancel = nullptr)
{
    if (memcmp(rec, "FILE", 4) != 0) return false;

    uint16_t usaOff = rd16(rec + 4);
    uint16_t usaCnt = rd16(rec + 6);
    if (usaCnt > 0 && usaOff + usaCnt * 2 <= recordSize) {
        uint16_t usn = rd16(rec + usaOff);
        for (uint16_t i = 1; i < usaCnt; ++i) {
            uint32_t pos = uint32_t(i) * sectorSize - 2;
            if (pos + 2 > recordSize) break;
            if (rd16(rec + pos) != usn) return false;  // torn record
            memcpy(rec + pos, rec + usaOff + i * 2, 2);
        }
    }

    uint16_t flags = rd16(rec + 0x16);
    out.inUse = (flags & 1) ? 1 : 0;
    out.isDir = (flags & 2) ? 1 : 0;
    out.recNo = rd32(rec + 0x2C);
    // Deleted records may contain stale attribute bytes; they are not scan input.
    if (!out.inUse) return false;

    uint32_t used = rd32(rec + 0x18);
    if (used < 0x30 || used > recordSize) used = recordSize;
    uint32_t attrOff = rd16(rec + 0x14);
    if (attrOff >= used) return false;

    int bestNs = -1;
    bool haveName = false;
    uint64_t dataReal = 0, dataAlloc = 0;
    bool haveData = false;

    uint32_t off = attrOff;
    while (off <= used && used - off >= 8) {
        checkCanceled(cancel);
        uint32_t type = rd32(rec + off);
        if (type == 0xFFFFFFFF) break;
        uint32_t len = rd32(rec + off + 4);
        if (!validAttribute(rec, used, off, len))
            throw std::runtime_error("Invalid MFT attribute length");
        uint8_t nonResident = rec[off + 8];

        if (type == 0x10 && !nonResident) {  // STANDARD_INFORMATION
            uint32_t vlen = rd32(rec + off + 0x10);
            uint16_t voff = rd16(rec + off + 0x14);
            if (voff < 24 || voff > len || vlen > len - voff)
                throw std::runtime_error("Invalid MFT attribute value");
            if (vlen >= 0x24) {
                out.ctime = static_cast<int64_t>(rd64(rec + off + voff + 0));
                out.mtime = static_cast<int64_t>(rd64(rec + off + voff + 8));
                out.atime = static_cast<int64_t>(rd64(rec + off + voff + 24));
                out.attrs = rd32(rec + off + voff + 0x20);
            }
        } else if (type == 0x80 && !nonResident && rec[off + 9] == 0) {  // unnamed $DATA
            uint32_t vlen = rd32(rec + off + 0x10);
            const uint16_t voff = rd16(rec + off + 0x14);
            if (voff < 24 || voff > len || vlen > len - voff)
                throw std::runtime_error("Invalid MFT data value");
            dataReal = vlen;
            dataAlloc = vlen;
            haveData = true;
        } else if (type == 0x80 && nonResident && rec[off + 9] == 0) {
            // Real/allocated sizes live here; FILE_NAME sizes can be stale or zero.
            dataAlloc = rd64(rec + off + 0x28);
            dataReal = rd64(rec + off + 0x30);
            haveData = true;
        } else if (type == 0x30 && !nonResident) {  // FILE_NAME
            uint32_t vlen = rd32(rec + off + 0x10);
            uint16_t voff = rd16(rec + off + 0x14);
            if (voff < 24 || voff > len || vlen > len - voff)
                throw std::runtime_error("Invalid MFT filename value");
            if (vlen >= 0x42) {
                const uint8_t* v = rec + off + voff;
                uint64_t parentRef = rd64(v);
                uint8_t nameLen = v[0x40];
                uint8_t ns = v[0x41];
                if (nameLen > 0 && 0x42 + uint32_t(nameLen) * 2 <= vlen) {
                    if (nsPriority(ns) > bestNs) {
                        bestNs = nsPriority(ns);
                        out.parent = uint32_t(parentRef & 0x0000FFFFFFFFFFFFull);
                        out.ns = ns;
                        out.size = rd64(v + 0x30);
                        out.fnSize = out.size;
                        out.alloc = rd64(v + 0x28);
                        if (out.mtime == 0) out.mtime = static_cast<int64_t>(rd64(v + 16));
                        if (out.ctime == 0) out.ctime = static_cast<int64_t>(rd64(v + 8));
                        if (out.atime == 0) out.atime = static_cast<int64_t>(rd64(v + 32));
                        out.nameOff = uint32_t(nameArena.size());
                        out.nameLen = nameLen;
                        nameArena.insert(nameArena.end(),
                                         reinterpret_cast<const char16_t*>(v + 0x42),
                                         reinterpret_cast<const char16_t*>(v + 0x42) + nameLen);
                        haveName = true;
                    }
                }
            }
        }
        off += len;
    }
    out.hasData = haveData ? 1 : 0;
    if (!out.isDir && haveData) {
        out.size = dataReal;
        out.alloc = dataAlloc ? dataAlloc : dataReal;
    }
    return haveName;
}

// ---------------------------------------------------------------- tree helpers

std::wstring volumeRootPath(wchar_t letter)
{
    wchar_t buf[8];
    swprintf_s(buf, L"%c:\\", letter);
    return buf;
}

std::wstring queryVolumeLabel(wchar_t letter)
{
    wchar_t root[8];
    swprintf_s(root, L"%c:\\", letter);
    wchar_t label[MAX_PATH + 1] = {};
    wchar_t fs[MAX_PATH + 1] = {};
    DWORD serial = 0, maxLen = 0, flags = 0;
    if (GetVolumeInformationW(root, label, MAX_PATH, &serial, &maxLen, &flags, fs, MAX_PATH)) {
        return label;
    }
    return L"";
}

void attachOrphans(Tree& t, std::atomic_bool* cancel)
{
    const uint32_t n = uint32_t(t.nodes.size());
    std::vector<uint8_t> state(n, 0);  // 0 unknown, 1 ok, 2 orphan
    for (uint32_t i = 0; i < n; ++i) {
        checkCanceled(cancel);
        if (i == t.root || state[i] == 1) continue;
        uint32_t cur = i;
        int guard = 0;
        bool ok = false;
        while (guard++ < 512) {
            if (cur == t.root) { ok = true; break; }
            if (cur >= n || t.nodes[cur].parent == kNone) break;
            cur = t.nodes[cur].parent;
        }
        if (!ok) {
            t.nodes[i].parent = t.root;
            state[i] = 2;
        }
    }
}

void buildFromRecords(std::vector<Rec>& recs, std::vector<char16_t>& arena,
                      Tree& tree, uint64_t& usedRecords, std::atomic_bool* cancel)
{
    // Record numbers may be sparse or corrupt; never allocate by the largest ID.
    QHash<uint32_t, uint32_t> map;
    tree.nodes.reserve(recs.size() + 1);
    tree.names.reserve(arena.size() + 64);

    Node root;
    root.isDir = 1;
    root.mftRec = 5;
    tree.nodes.push_back(root);
    tree.root = 0;

    auto addName = [&](const char16_t* src, uint32_t len) {
        uint32_t off = uint32_t(tree.names.size());
        tree.names.insert(tree.names.end(), src, src + len);
        return off;
    };

    // root name is patched by caller later (path string), placeholder here
    usedRecords = 0;
    for (const Rec& r : recs) {
        checkCanceled(cancel);
        if (!r.inUse || r.nameLen == 0) continue;
        if (r.recNo == 5) continue;  // root record
        Node nd;
        nd.size = r.size;
        nd.alloc = r.alloc;
        nd.mtime = r.mtime;
        nd.ctime = r.ctime;
        nd.atime = r.atime;
        nd.attrs = r.attrs;
        nd.isDir = r.isDir;
        nd.mftRec = r.recNo;
        nd.nameOff = addName(arena.data() + r.nameOff, r.nameLen);
        nd.nameLen = r.nameLen;
        tree.nodes.push_back(nd);
        map[r.recNo] = uint32_t(tree.nodes.size() - 1);
        ++usedRecords;
    }

    // second pass over recs to link parents (recs order == node order for accepted ones)
    size_t nodeIdx = 1;
    for (const Rec& r : recs) {
        checkCanceled(cancel);
        if (!r.inUse || r.nameLen == 0) continue;
        if (r.recNo == 5) continue;
        if (nodeIdx >= tree.nodes.size()) break;
        uint32_t p = r.parent;
        if (p == kNone || !map.contains(p) || map.value(p) == nodeIdx)
            tree.nodes[nodeIdx].parent = 0;
        else
            tree.nodes[nodeIdx].parent = map.value(p);
        ++nodeIdx;
    }
    attachOrphans(tree, cancel);
}

void setRootName(Tree& tree, const QString& name)
{
    if (tree.nodes.empty()) return;
    Node& r = tree.nodes[0];
    const char16_t* p = reinterpret_cast<const char16_t*>(name.utf16());
    r.nameOff = 0;
    r.nameLen = uint32_t(name.size());
    tree.names.insert(tree.names.begin(), p, p + name.size());
    for (uint32_t i = 1; i < tree.nodes.size(); ++i) tree.nodes[i].nameOff += r.nameLen;
}

}  // namespace

// ---------------------------------------------------------------- Tree

QString Tree::name(uint32_t i) const
{
    if (i >= nodes.size()) return {};
    const Node& n = nodes[i];
    if (n.nameLen == 0) return {};
    return QString::fromUtf16(names.data() + n.nameOff, n.nameLen);
}

QString Tree::path(uint32_t i) const
{
    QStringList parts;
    uint32_t cur = i;
    int guard = 0;
    while (cur != kNone && cur < nodes.size() && guard++ < 512) {
        const QString part = name(cur);
        parts.prepend(part);
        if (cur == root || QDir::isAbsolutePath(QDir::fromNativeSeparators(part))) break;
        cur = nodes[cur].parent;
    }
    QString p;
    for (const auto& part : parts) {
        if (!p.isEmpty() && !p.endsWith('\\') && !p.endsWith('/')) p += QLatin1Char('\\');
        p += part;
    }
    return QDir::toNativeSeparators(p);
}

void Tree::finalize(std::atomic_bool* cancel)
{
    checkCanceled(cancel);
    liveChildren.clear();
    const uint32_t n = uint32_t(nodes.size());
    if (n == 0) return;
    std::vector<uint32_t> cnt(n, 0);
    for (uint32_t i = 0; i < n; ++i) {
        checkCanceled(cancel);
        uint32_t p = nodes[i].parent;
        if (p != kNone && p < n && p != i) cnt[p]++;
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        checkCanceled(cancel);
        nodes[i].childBegin = total;
        nodes[i].childCount = cnt[i];
        nodes[i].childCapacity = cnt[i];
        total += cnt[i];
    }
    children.assign(total, 0);
    std::vector<uint32_t> cursor(n);
    for (uint32_t i = 0; i < n; ++i) {
        checkCanceled(cancel);
        cursor[i] = nodes[i].childBegin;
    }
    for (uint32_t i = 0; i < n; ++i) {
        checkCanceled(cancel);
        uint32_t p = nodes[i].parent;
        if (p == kNone || p >= n || p == i) continue;
        uint32_t pos = cursor[p]++;
        children[pos] = i;
        nodes[i].rowInParent = pos - nodes[p].childBegin;
    }

    // BFS order from root
    std::vector<uint32_t> order;
    order.reserve(n);
    std::vector<uint8_t> seen(n, 0);
    if (root != kNone && root < n) {
        order.push_back(root);
        seen[root] = 1;
    }
    for (size_t k = 0; k < order.size(); ++k) {
        const Node& nd = nodes[order[k]];
        for (uint32_t j = 0; j < nd.childCount; ++j) {
            checkCanceled(cancel);
            uint32_t c = children[nd.childBegin + j];
            if (!seen[c]) {
                seen[c] = 1;
                order.push_back(c);
            }
        }
    }

    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        checkCanceled(cancel);
        Node& nd = nodes[*it];
        uint64_t s = 0, a = 0;
        uint32_t f = 0, d = 0;
        for (uint32_t j = 0; j < nd.childCount; ++j) {
            checkCanceled(cancel);
            const Node& c = nodes[children[nd.childBegin + j]];
            s += c.size;
            a += c.alloc;
            f += c.files;
            d += c.folders;
            if (c.isDir) ++d; else ++f;
        }
        nd.size += s;
        nd.alloc += a;
        nd.files = f;
        nd.folders = d;
    }

    sortRecursive(2, false, cancel);
    checkCanceled(cancel);
}

void Tree::sortRecursive(int column, bool ascending, std::atomic_bool* cancel)
{
    if (root == kNone || root >= nodes.size()) return;
    std::vector<uint32_t> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        checkCanceled(cancel);
        const uint32_t n = stack.back();
        stack.pop_back();
        sortChildrenBy(n, column, ascending, cancel);
        const Node& nd = nodes[n];
        for (uint32_t i = 0; i < nd.childCount; ++i) {
            checkCanceled(cancel);
            stack.push_back(children[nd.childBegin + i]);
        }
    }
}

void Tree::sortChildrenBy(uint32_t node, int column, bool ascending, std::atomic_bool* cancel)
{
    checkCanceled(cancel);
    if (node >= nodes.size()) return;
    Node& nd = nodes[node];
    if (nd.childCount < 2) return;
    auto key = [&](uint32_t a, uint32_t b) {
        checkCanceled(cancel);
        const Node& x = nodes[a];
        const Node& y = nodes[b];
        switch (column) {
        case 0: {
            int c = QString::compare(name(a), name(b), Qt::CaseInsensitive);
            return ascending ? c < 0 : c > 0;
        }
        case 2: return ascending ? x.size < y.size : x.size > y.size;
        case 3: return ascending ? x.alloc < y.alloc : x.alloc > y.alloc;
        case 5: return ascending ? x.files < y.files : x.files > y.files;
        case 6: return ascending ? x.folders < y.folders : x.folders > y.folders;
        case 7: return ascending ? x.mtime < y.mtime : x.mtime > y.mtime;
        default: return ascending ? x.size < y.size : x.size > y.size;
        }
    };
    std::sort(children.begin() + nd.childBegin, children.begin() + nd.childBegin + nd.childCount,
              [&](uint32_t a, uint32_t b) { return key(a, b); });
    for (uint32_t j = 0; j < nd.childCount; ++j) {
        checkCanceled(cancel);
        nodes[children[nd.childBegin + j]].rowInParent = j;
    }
}

// ---------------------------------------------------------------- MFT scan

static ScanResult scanNtfsImpl(wchar_t letter, const ScanOptions& opt)
{
    checkCanceled(opt.cancel);
    ScanResult res;
    QElapsedTimer timer;
    timer.start();

    Volume vol;
    if (!vol.open(letter)) {
        res.error = QStringLiteral("Cannot open volume (error %1) — administrator rights required")
                        .arg(vol.error());
        return res;
    }
    if (!vol.readBoot()) {
        res.error = QStringLiteral("Cannot read NTFS boot sector");
        return res;
    }
    const uint32_t clusterSize = vol.clusterSize();
    const uint32_t recordSize = vol.recordSize();
    const uint32_t sectorSize = vol.bytesPerSector();

    if (opt.status) opt.status(QStringLiteral("Reading $MFT..."));

    std::vector<uint8_t> rec0(recordSize);
    if (!vol.readAt(vol.mftLcn() * clusterSize, rec0.data(), recordSize)) {
        res.error = QStringLiteral("Cannot read $MFT record 0");
        return res;
    }

    // fixups on record 0
    uint16_t usaOff = rd16(rec0.data() + 4);
    uint16_t usaCnt = rd16(rec0.data() + 6);
    if (usaCnt && usaOff + usaCnt * 2 <= recordSize) {
        uint16_t usn = rd16(rec0.data() + usaOff);
        for (uint16_t i = 1; i < usaCnt; ++i) {
            uint32_t pos = uint32_t(i) * sectorSize - 2;
            if (pos + 2 > recordSize) break;
            if (rd16(rec0.data() + pos) != usn) break;
            memcpy(rec0.data() + pos, rec0.data() + usaOff + i * 2, 2);
        }
    }

    std::vector<Run> runs;
    uint64_t dataSize = 0, allocSize = 0;
    {
        uint32_t off = rd16(rec0.data() + 0x14);
        uint32_t used = rd32(rec0.data() + 0x18);
        if (used > recordSize) used = recordSize;
        while (off <= used && used - off >= 8) {
            checkCanceled(opt.cancel);
            uint32_t type = rd32(rec0.data() + off);
            if (type == 0xFFFFFFFF) break;
            uint32_t len = rd32(rec0.data() + off + 4);
            if (!validAttribute(rec0.data(), used, off, len))
                throw std::runtime_error("Invalid MFT runlist attribute");
            if (type == 0x80 && rec0[off + 8]) {  // DATA, non-resident
                uint64_t realSize = rd64(rec0.data() + off + 0x30);
                uint64_t allocSz = rd64(rec0.data() + off + 0x28);
                uint16_t runOff = rd16(rec0.data() + off + 0x20);
                if (runOff >= 64 && runOff < len) {
                    dataSize = realSize;
                    allocSize = allocSz;
                    parseRunList(rec0.data() + off + runOff, len - runOff, runs, dataSize, allocSize);
                }
            }
            off += len;
        }
    }
    if (runs.empty() || dataSize == 0) {
        res.error = QStringLiteral("Cannot parse $MFT runlist");
        return res;
    }

    const uint64_t recordCount = std::min<uint64_t>(dataSize / recordSize, 64ull * 1024 * 1024);
    if (opt.status) opt.status(QStringLiteral("Scanning MFT: %1 records").arg(recordCount));

    std::vector<Rec> recs;
    recs.reserve(size_t(std::min<uint64_t>(recordCount, 65536)));
    std::vector<char16_t> arena;
    arena.reserve(size_t(std::min<uint64_t>(recordCount, 65536)) * 8);

    const uint64_t chunkBytes = 4ull << 20;
    std::vector<uint8_t> buf(static_cast<size_t>(chunkBytes));
    uint64_t index = 0;
    uint64_t lastStatus = 0;

    while (index < recordCount) {
        if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) {
            res.error = QStringLiteral("canceled");
            return res;
        }
        uint64_t fileOff = index * recordSize;
        uint64_t phys = 0, avail = 0;
        if (!mapOffset(runs, clusterSize, fileOff, phys, avail)) break;
        uint64_t nrec = std::min<uint64_t>({recordCount - index, avail / recordSize,
                                            chunkBytes / recordSize});
        if (nrec == 0) break;
        DWORD bytes = DWORD(nrec * recordSize);
        if (!vol.readAt(phys, buf.data(), bytes)) break;
        for (uint64_t k = 0; k < nrec; ++k) {
            Rec r;
            checkCanceled(opt.cancel);
            if (parseRecord(buf.data() + k * recordSize, recordSize, sectorSize, arena, r, opt.cancel))
                recs.push_back(r);
        }
        index += nrec;
        if (opt.status && index - lastStatus >= 200000) {
            lastStatus = index;
            opt.status(QStringLiteral("Scanning MFT: %1 / %2").arg(index).arg(recordCount));
        }
    }

    res.records = index;
    res.parsed = recs.size();
    for (const Rec& r : recs) {
        if (r.isDir || r.nameLen == 0) continue;
        res.sumDataSizes += r.size;
        res.sumFileNameSizes += r.fnSize;
        if (!r.hasData) ++res.noDataFiles;
        if (r.size == 0) ++res.zeroSizeFiles;
    }
    if (opt.status) opt.status(QStringLiteral("Building tree..."));
    uint64_t builtNodes = 0;
    buildFromRecords(recs, arena, res.tree, builtNodes, opt.cancel);
    res.parsed = builtNodes;
    setRootName(res.tree, QString::fromWCharArray((std::wstring(1, letter) + L":\\").c_str()));
    res.tree.finalize(opt.cancel);
    res.ok = true;
    res.usesMft = true;
    res.seconds = timer.elapsed() / 1000.0;
    return res;
}

// ---------------------------------------------------------------- folder scan

struct PendingDir {
    uint32_t node;
    std::wstring path;
};

static ScanResult scanFolderImpl(const QString& rootPath, const ScanOptions& opt)
{
    checkCanceled(opt.cancel);
    ScanResult res;
    QElapsedTimer timer;
    timer.start();

    std::wstring root = QDir::toNativeSeparators(rootPath).toStdWString();
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (root.empty()) root = L"C:";
    if (GetFileAttributesW(longPath(root).c_str()) == INVALID_FILE_ATTRIBUTES) {
        res.error = QStringLiteral("Path not found: %1").arg(QString::fromStdWString(root));
        return res;
    }

    Tree& tree = res.tree;
    tree.nodes.reserve(1 << 16);
    tree.names.reserve(1 << 18);

    Node rn;
    rn.isDir = 1;
    tree.nodes.push_back(rn);
    tree.root = 0;
    {
        QString display = QString::fromStdWString(root);
        const char16_t* p = reinterpret_cast<const char16_t*>(display.utf16());
        tree.names.insert(tree.names.end(), p, p + display.size());
        tree.nodes[0].nameOff = 0;
        tree.nodes[0].nameLen = uint32_t(display.size());
    }

    uint32_t clusterSize = 4096;
    {
        wchar_t volRoot[MAX_PATH + 1] = {};
        if (GetVolumePathNameW(root.c_str(), volRoot, MAX_PATH)) {
            DWORD spc = 0, bps = 0, freeClusters = 0, totalClusters = 0;
            if (GetDiskFreeSpaceW(volRoot, &spc, &bps, &freeClusters, &totalClusters) && spc && bps)
                clusterSize = spc * bps;
        }
    }

    std::vector<PendingDir> stack;
    stack.push_back({0, root});
    uint64_t visited = 0;

    while (!stack.empty()) {
        if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) {
            res.error = QStringLiteral("canceled");
            return res;
        }
        PendingDir cur = std::move(stack.back());
        stack.pop_back();

        std::wstring pattern = longPath(cur.path) + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindHandle findHandle{h};

        do {
            checkCanceled(opt.cancel);
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                continue;
            QString name = QString::fromWCharArray(fd.cFileName);
            Node nd;
            nd.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
            nd.attrs = fd.dwFileAttributes;
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            nd.size = nd.isDir ? 0 : sz.QuadPart;
            nd.alloc = nd.isDir ? 0
                                : ((nd.size + clusterSize - 1) / clusterSize) * clusterSize;
            nd.mtime = (int64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                       fd.ftLastWriteTime.dwLowDateTime;
            nd.ctime = (int64_t(fd.ftCreationTime.dwHighDateTime) << 32) |
                       fd.ftCreationTime.dwLowDateTime;
            nd.atime = (int64_t(fd.ftLastAccessTime.dwHighDateTime) << 32) |
                       fd.ftLastAccessTime.dwLowDateTime;
            nd.parent = cur.node;
            nd.nameOff = uint32_t(tree.names.size());
            nd.nameLen = uint32_t(name.size());
            const char16_t* p = reinterpret_cast<const char16_t*>(name.utf16());
            tree.names.insert(tree.names.end(), p, p + name.size());
            tree.nodes.push_back(nd);
            uint32_t idx = uint32_t(tree.nodes.size() - 1);
            ++visited;

            if (nd.isDir) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back({idx, cur.path + L"\\" + fd.cFileName});
            }
            if (opt.status && (visited % 20000) == 0)
                opt.status(QStringLiteral("Scanning: %1 items").arg(visited));
            checkCanceled(opt.cancel);
        } while (FindNextFileW(h, &fd));
    }

    if (opt.status) opt.status(QStringLiteral("Building tree..."));
    tree.finalize(opt.cancel);
    res.ok = true;
    res.records = visited;
    res.seconds = timer.elapsed() / 1000.0;
    res.usesMft = false;
    return res;
}

// ---------------------------------------------------------------- public API

// ---------------------------------------------------------------- MFT dumps

namespace {

constexpr char kMftMagic[8] = {'W', 'T', 'M', 'F', 'T', 0, 0, 1};

void putU16(std::vector<uint8_t>& r, size_t off, uint16_t v) { memcpy(r.data() + off, &v, 2); }
void putU32(std::vector<uint8_t>& r, size_t off, uint32_t v) { memcpy(r.data() + off, &v, 4); }
void putU64(std::vector<uint8_t>& r, size_t off, uint64_t v) { memcpy(r.data() + off, &v, 8); }

void buildSyntheticRecord(std::vector<uint8_t>& rec, uint32_t recNo, uint32_t parent,
                          const std::u16string& name, bool isDir, uint64_t size)
{
    const uint32_t recordSize = 1024;
    rec.assign(recordSize, 0);
    memcpy(rec.data(), "FILE", 4);
    putU16(rec, 4, 0x30);
    putU16(rec, 6, 3);
    putU16(rec, 0x16, uint16_t(isDir ? 3 : 1));
    putU32(rec, 0x1C, recordSize);
    putU32(rec, 0x2C, recNo);
    putU16(rec, 0x14, 0x38);

    uint32_t off = 0x38;
    putU32(rec, off, 0x10);
    putU32(rec, off + 4, 0x60);
    putU16(rec, off + 0x10, 0x48);
    putU16(rec, off + 0x14, 0x18);
    putU64(rec, off + 0x18 + 8, 133000000000000000ull);
    putU64(rec, off + 0x18 + 24, 133000000000000000ull);
    putU32(rec, off + 0x18 + 0x20, uint32_t(isDir ? 0x10 : 0x20));
    off += 0x60;

    const uint32_t nameLen = uint32_t(name.size());
    const uint32_t valueLen = 0x42 + nameLen * 2;
    const uint32_t attrLen = (0x18 + valueLen + 7) & ~7u;
    putU32(rec, off, 0x30);
    putU32(rec, off + 4, attrLen);
    putU16(rec, off + 0x10, uint16_t(valueLen));
    putU16(rec, off + 0x14, 0x18);
    const size_t v = off + 0x18;
    putU64(rec, v, parent);
    putU64(rec, v + 8, 133000000000000000ull);
    putU64(rec, v + 16, 133000000000000000ull);
    putU64(rec, v + 24, 133000000000000000ull);
    putU64(rec, v + 32, 133000000000000000ull);
    putU64(rec, v + 0x28, size);
    putU64(rec, v + 0x30, size);
    rec[v + 0x40] = uint8_t(nameLen);
    rec[v + 0x41] = 1;
    memcpy(rec.data() + v + 0x42, name.data(), nameLen * 2);
    off += attrLen;
    putU32(rec, off, 0xFFFFFFFFu);
    putU32(rec, 0x18, off + 4);

    putU16(rec, 0x30, 1);
    putU16(rec, 0x32, 0x1111);
    putU16(rec, 0x34, 0x2222);
    putU16(rec, 510, 1);
    putU16(rec, 1022, 1);
}

}  // namespace

bool writeSyntheticMft(const QString& path, int recordCount, QString& error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    QByteArray header(kMftMagic, 8);
    uint32_t recordSize = 1024;
    uint64_t count = uint64_t(recordCount) + 11;
    header.append(reinterpret_cast<const char*>(&recordSize), 4);
    header.append(reinterpret_cast<const char*>(&count), 8);
    f.write(header);

    std::vector<uint8_t> rec;
    auto writeRec = [&](uint32_t recNo, uint32_t parent, const char* name, bool isDir, uint64_t size) {
        std::string n(name);
        buildSyntheticRecord(rec, recNo, parent, std::u16string(n.begin(), n.end()), isDir, size);
        f.write(reinterpret_cast<const char*>(rec.data()), rec.size());
    };

    // record 0..4 unused; record 5 is the root directory
    for (uint32_t i = 0; i < 5; ++i) writeRec(i, 5, "", true, 0);
    writeRec(5, 5, "ROOT", true, 0);
    writeRec(6, 5, "Windows", true, 0);
    writeRec(7, 5, "Users", true, 0);
    writeRec(8, 6, "System32", true, 0);
    writeRec(9, 6, "Temp", true, 0);
    writeRec(10, 7, "alice", true, 0);
    for (int i = 0; i < recordCount; ++i) {
        const uint32_t parent = 8u + uint32_t(i % 3);
        const uint64_t size = 1024ull * uint64_t(1 + (i * 37) % 4096);
        const std::string nm = "file_" + std::to_string(i) + ".dat";
        buildSyntheticRecord(rec, 100u + uint32_t(i), parent, std::u16string(nm.begin(), nm.end()),
                             false, size);
        f.write(reinterpret_cast<const char*>(rec.data()), rec.size());
    }
    f.close();
    return true;
}

static ScanResult scanMftDumpImpl(const QString& path, const ScanOptions& opt)
{
    checkCanceled(opt.cancel);
    ScanResult res;
    QElapsedTimer timer;
    timer.start();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        res.error = QStringLiteral("Cannot open %1").arg(path);
        return res;
    }
    char magic[8] = {};
    if (f.read(magic, 8) != 8) {
        res.error = QStringLiteral("File too small");
        return res;
    }
    uint32_t recordSize = 1024;
    uint64_t count = 0;
    if (memcmp(magic, kMftMagic, 8) == 0) {
        uint32_t rs = 0;
        uint64_t cnt = 0;
        if (f.read(reinterpret_cast<char*>(&rs), 4) != 4 ||
            f.read(reinterpret_cast<char*>(&cnt), 8) != 8) {
            res.error = QStringLiteral("Truncated header");
            return res;
        }
        recordSize = rs;
        count = cnt;
    } else {
        f.seek(0);
        count = uint64_t(f.size()) / recordSize;
    }
    if (recordSize < 512 || recordSize > 65536) {
        res.error = QStringLiteral("Unsupported record size %1").arg(recordSize);
        return res;
    }
    const uint64_t availableRecords = uint64_t(f.size() - f.pos()) / recordSize;
    if (count > availableRecords || count > 64ull * 1024 * 1024) {
        res.error = QStringLiteral("Invalid or excessive MFT record count");
        return res;
    }

    std::vector<Rec> recs;
    recs.reserve(size_t(std::min<uint64_t>(count, 65536)));
    std::vector<char16_t> arena;
    arena.reserve(size_t(std::min<uint64_t>(count, 65536)) * 8);
    std::vector<uint8_t> buf;
    buf.resize(static_cast<size_t>(recordSize));

    for (uint64_t i = 0; i < count; ++i) {
        if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) {
            res.error = QStringLiteral("canceled");
            return res;
        }
        if (f.read(reinterpret_cast<char*>(buf.data()), recordSize) != qint64(recordSize))
            throw std::runtime_error("Truncated MFT record");
        Rec r;
        if (parseRecord(buf.data(), recordSize, 512, arena, r, opt.cancel)) {
            recs.push_back(r);
        }
        if (opt.status && (i % 200000) == 0 && i > 0)
            opt.status(QStringLiteral("Reading MFT dump: %1 / %2").arg(i).arg(count));
    }
    res.records = recs.size();
    for (const Rec& r : recs) {
        if (r.isDir || r.nameLen == 0) continue;
        res.sumDataSizes += r.size;
        res.sumFileNameSizes += r.fnSize;
        if (!r.hasData) ++res.noDataFiles;
        if (r.size == 0) ++res.zeroSizeFiles;
    }
    if (opt.status) opt.status(QStringLiteral("Building tree..."));
    uint64_t builtNodes = 0;
    buildFromRecords(recs, arena, res.tree, builtNodes, opt.cancel);
    res.parsed = builtNodes;
    setRootName(res.tree, QFileInfo(path).completeBaseName());
    res.tree.finalize(opt.cancel);
    res.ok = true;
    res.usesMft = true;
    res.seconds = timer.elapsed() / 1000.0;
    return res;
}

static ScanResult dumpMftToFileImpl(wchar_t letter, const QString& outputPath, std::atomic_bool& cancel,
                         const StatusFn& status)
{
    checkCanceled(&cancel);
    if (status) status(QStringLiteral("Reading $MFT..."));
    ScanResult res;
    Volume vol;
    if (!vol.open(letter)) {
        res.error = QStringLiteral("Cannot open volume (error %1)").arg(vol.error());
        return res;
    }
    if (!vol.readBoot()) {
        res.error = QStringLiteral("Cannot read NTFS boot sector");
        return res;
    }
    const uint32_t clusterSize = vol.clusterSize();
    const uint32_t recordSize = vol.recordSize();
    const uint32_t sectorSize = vol.bytesPerSector();

    std::vector<uint8_t> rec0(recordSize);
    if (!vol.readAt(vol.mftLcn() * clusterSize, rec0.data(), recordSize)) {
        res.error = QStringLiteral("Cannot read $MFT record 0");
        return res;
    }
    uint16_t usaOff = rd16(rec0.data() + 4);
    uint16_t usaCnt = rd16(rec0.data() + 6);
    if (usaCnt && usaOff + usaCnt * 2 <= recordSize) {
        uint16_t usn = rd16(rec0.data() + usaOff);
        for (uint16_t i = 1; i < usaCnt; ++i) {
            uint32_t pos = uint32_t(i) * sectorSize - 2;
            if (pos + 2 > recordSize) break;
            if (rd16(rec0.data() + pos) != usn) break;
            memcpy(rec0.data() + pos, rec0.data() + usaOff + i * 2, 2);
        }
    }
    std::vector<Run> runs;
    uint64_t dataSize = 0, allocSize = 0;
    uint32_t off = rd16(rec0.data() + 0x14);
    uint32_t used = rd32(rec0.data() + 0x18);
    if (used > recordSize) used = recordSize;
    while (off <= used && used - off >= 8) {
        checkCanceled(&cancel);
        uint32_t type = rd32(rec0.data() + off);
        if (type == 0xFFFFFFFF) break;
        uint32_t len = rd32(rec0.data() + off + 4);
        if (!validAttribute(rec0.data(), used, off, len))
            throw std::runtime_error("Invalid MFT runlist attribute");
        if (type == 0x80 && rec0[off + 8]) {
            dataSize = rd64(rec0.data() + off + 0x30);
            allocSize = rd64(rec0.data() + off + 0x28);
            uint16_t runOff = rd16(rec0.data() + off + 0x20);
            if (runOff >= 64 && runOff < len)
                parseRunList(rec0.data() + off + runOff, len - runOff, runs, dataSize, allocSize);
        }
        off += len;
    }
    if (runs.empty() || dataSize == 0) {
        res.error = QStringLiteral("Cannot parse $MFT runlist");
        return res;
    }

    QSaveFile out(outputPath);
    if (!out.open(QIODevice::WriteOnly)) {
        res.error = QStringLiteral("Cannot write %1").arg(outputPath);
        return res;
    }
    const uint64_t count = dataSize / recordSize;
    QByteArray header(kMftMagic, 8);
    header.append(reinterpret_cast<const char*>(&recordSize), 4);
    header.append(reinterpret_cast<const char*>(&count), 8);
    if (out.write(header) != header.size()) throw std::runtime_error("Cannot write MFT header");

    const uint64_t chunk = 4ull << 20;
    std::vector<uint8_t> buf;
    buf.resize(static_cast<size_t>(chunk));
    uint64_t index = 0;
    QElapsedTimer progressTimer;
    progressTimer.start();
    while (index < count) {
        checkCanceled(&cancel);
        uint64_t fileOff = index * recordSize;
        uint64_t phys = 0, avail = 0;
        if (!mapOffset(runs, clusterSize, fileOff, phys, avail))
            throw std::runtime_error("Incomplete MFT runlist");
        uint64_t nrec = std::min<uint64_t>({count - index, avail / recordSize, chunk / recordSize});
        if (nrec == 0) throw std::runtime_error("Invalid MFT run length");
        DWORD bytes = DWORD(nrec * recordSize);
        if (!vol.readAt(phys, buf.data(), bytes)) throw std::runtime_error("Cannot read MFT data");
        checkCanceled(&cancel);
        if (out.write(reinterpret_cast<const char*>(buf.data()), bytes) != bytes)
            throw std::runtime_error("Cannot write MFT data");
        index += nrec;
        if (status && (progressTimer.elapsed() >= 100 || index == count)) {
            status(QStringLiteral("Dumping MFT: %1 / %2").arg(index).arg(count));
            progressTimer.restart();
        }
    }
    checkCanceled(&cancel);
    if (!out.commit()) throw std::runtime_error("Cannot save MFT dump");
    res.records = index;
    res.ok = true;
    res.usesMft = true;
    return res;
}

// ---------------------------------------------------------------- merging

Tree mergeTrees(std::vector<Tree>& parts, const QString& rootName, std::atomic_bool* cancel)
{
    checkCanceled(cancel);
    Tree out;
    Node root;
    root.isDir = 1;
    out.nodes.push_back(root);
    out.root = 0;
    for (Tree& part : parts) {
        checkCanceled(cancel);
        if (part.nodes.empty() || part.root >= part.nodes.size()) continue;
        const uint32_t nodeBase = uint32_t(out.nodes.size());
        const uint32_t nameBase = uint32_t(out.names.size());
        out.names.insert(out.names.end(), part.names.begin(), part.names.end());
        for (uint32_t i = 0; i < part.nodes.size(); ++i) {
            checkCanceled(cancel);
            Node nd = part.nodes[i];
            nd.nameOff += nameBase;
            if (nd.isDir) {
                // Directory totals are recomputed by finalize(); the source tree already
                // holds aggregated values, so reset them to avoid double counting.
                nd.size = 0;
                nd.alloc = 0;
            }
            if (nd.parent == kNone)
                nd.parent = 0;
            else
                nd.parent += nodeBase;
            out.nodes.push_back(nd);
        }
        out.nodes[nodeBase + part.root].parent = 0;
    }
    const QString name = rootName.isEmpty() ? QStringLiteral("Multiple selections") : rootName;
    out.nodes[0].nameOff = uint32_t(out.names.size());
    out.nodes[0].nameLen = uint32_t(name.size());
    const char16_t* p = reinterpret_cast<const char16_t*>(name.utf16());
    out.names.insert(out.names.end(), p, p + name.size());
    out.finalize(cancel);
    return out;
}

// ---------------------------------------------------------------- CSV import

namespace {

QStringList splitDelimited(const QString& line, QChar delim)
{
    QStringList out;
    QString cur;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    cur += QLatin1Char('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cur += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == delim) {
            out << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    out << cur;
    return out;
}

uint64_t parseU64Field(const QString& s)
{
    bool ok = false;
    const uint64_t v = s.trimmed().toULongLong(&ok);
    return ok ? v : 0;
}

int64_t parseTimeField(const QString& s)
{
    const QString v = s.trimmed();
    if (v.isEmpty()) return 0;
    QDateTime dt = QDateTime::fromString(v, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!dt.isValid()) dt = QDateTime::fromString(v, QStringLiteral("yyyy-MM-dd HH:mm"));
    if (!dt.isValid()) return 0;
    return dt.toMSecsSinceEpoch() * 10000 + 116444736000000000LL;
}

uint32_t parseAttrField(const QString& s)
{
    uint32_t a = 0;
    for (QChar c : s) {
        switch (c.toUpper().unicode()) {
        case 'R': a |= FILE_ATTRIBUTE_READONLY; break;
        case 'H': a |= FILE_ATTRIBUTE_HIDDEN; break;
        case 'S': a |= FILE_ATTRIBUTE_SYSTEM; break;
        case 'A': a |= FILE_ATTRIBUTE_ARCHIVE; break;
        case 'C': a |= FILE_ATTRIBUTE_COMPRESSED; break;
        case 'E': a |= FILE_ATTRIBUTE_ENCRYPTED; break;
        case 'T': a |= FILE_ATTRIBUTE_TEMPORARY; break;
        case 'O': a |= FILE_ATTRIBUTE_OFFLINE; break;
        default: break;
        }
    }
    return a;
}

}  // namespace

static ScanResult importCsvTreeImpl(const QString& path, const ScanOptions& opt)
{
    checkCanceled(opt.cancel);
    ScanResult res;
    QElapsedTimer timer;
    timer.start();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        res.error = QStringLiteral("Cannot open %1").arg(path);
        return res;
    }
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);

    const QChar delim = path.endsWith(QLatin1String(".tsv"), Qt::CaseInsensitive)
                            ? QLatin1Char('\t')
                            : QLatin1Char(',');
    QString header = in.readLine();
    if (header.startsWith(QChar(0xFEFF))) header.remove(0, 1);
    const QStringList cols = splitDelimited(header, delim);
    int nameIdx = -1, pathIdx = -1, sizeIdx = -1, allocIdx = -1, mtimeIdx = -1, attrIdx = -1;
    int filesIdx = -1, foldersIdx = -1;
    for (int i = 0; i < cols.size(); ++i) {
        const QString c = cols.at(i).trimmed().toLower();
        if (c == QLatin1String("file name")) nameIdx = i;
        else if (c == QLatin1String("path")) pathIdx = i;
        else if (c == QLatin1String("size")) sizeIdx = i;
        else if (c == QLatin1String("allocated")) allocIdx = i;
        else if (c == QLatin1String("modified")) mtimeIdx = i;
        else if (c == QLatin1String("attributes")) attrIdx = i;
        else if (c == QLatin1String("files")) filesIdx = i;
        else if (c == QLatin1String("folders")) foldersIdx = i;
    }
    if (nameIdx < 0 || pathIdx < 0) {
        res.error = QStringLiteral("CSV must contain 'File Name' and 'Path' columns");
        return res;
    }

    Tree& tree = res.tree;
    tree.nodes.reserve(1u << 16);
    tree.names.reserve(1u << 18);
    {
        Node root;
        root.isDir = 1;
        tree.nodes.push_back(root);
        tree.root = 0;
    }

    QHash<QString, uint32_t> dirMap;
    QString rootPath;
    uint64_t rows = 0;

    auto addName = [&](const QString& n) {
        const uint32_t off = uint32_t(tree.names.size());
        const char16_t* p = reinterpret_cast<const char16_t*>(n.utf16());
        tree.names.insert(tree.names.end(), p, p + n.size());
        return off;
    };

    std::function<uint32_t(const QString&)> ensureDir = [&](const QString& raw) -> uint32_t {
        checkCanceled(opt.cancel);
        QString p = raw;
        while (p.endsWith(QLatin1Char('\\'))) p.chop(1);
        if (p.isEmpty()) return 0;
        if (!rootPath.isEmpty() && p.compare(rootPath, Qt::CaseInsensitive) == 0) return 0;
        const QString key = p.toLower();
        const auto it = dirMap.constFind(key);
        if (it != dirMap.constEnd()) return it.value();

        const int slash = p.lastIndexOf(QLatin1Char('\\'));
        QString parent;
        QString base;
        if (slash > 2) {
            base = p.mid(slash + 1);
            parent = p.left(slash);
        } else {
            base = p;
        }
        const uint32_t parentIdx = parent.isEmpty() ? 0 : ensureDir(parent);
        Node nd;
        nd.isDir = 1;
        nd.parent = parentIdx;
        nd.nameOff = addName(base);
        nd.nameLen = uint32_t(base.size());
        tree.nodes.push_back(nd);
        const uint32_t idx = uint32_t(tree.nodes.size() - 1);
        dirMap.insert(key, idx);
        return idx;
    };

    while (!in.atEnd()) {
        if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) {
            res.error = QStringLiteral("canceled");
            return res;
        }
        const QString line = in.readLine();
        if (line.isEmpty()) continue;
        if (opt.status && (rows % 100000) == 0 && rows > 0)
            opt.status(QStringLiteral("Importing CSV: %1 rows").arg(rows));
        const QStringList f2 = splitDelimited(line, delim);
        if (f2.size() <= qMax(nameIdx, pathIdx)) continue;
        const QString name = f2.at(nameIdx).trimmed();
        if (name.isEmpty()) continue;
        const QString parentPath = f2.at(pathIdx).trimmed();
        if (rootPath.isEmpty()) rootPath = parentPath;

        const uint32_t parentIdx = ensureDir(parentPath);
        const uint32_t rowFiles = filesIdx >= 0 && filesIdx < f2.size() ? uint32_t(parseU64Field(f2.at(filesIdx))) : 0;
        const uint32_t rowFolders = foldersIdx >= 0 && foldersIdx < f2.size() ? uint32_t(parseU64Field(f2.at(foldersIdx))) : 0;
        const bool looksDir = rowFiles > 0 || rowFolders > 0;
        const QString fullKey =
            (parentPath.isEmpty() ? name : parentPath + QLatin1Char('\\') + name).toLower();
        if (dirMap.contains(fullKey)) {
            // The directory already exists because a child row referenced it first.
            const uint32_t existing = dirMap.value(fullKey);
            if (mtimeIdx >= 0 && mtimeIdx < f2.size())
                tree.nodes[existing].mtime = parseTimeField(f2.at(mtimeIdx));
            if (attrIdx >= 0 && attrIdx < f2.size())
                tree.nodes[existing].attrs = parseAttrField(f2.at(attrIdx));
            continue;
        }
        Node nd;
        nd.isDir = looksDir ? 1 : 0;
        nd.parent = parentIdx;
        nd.size = sizeIdx >= 0 && sizeIdx < f2.size() ? parseU64Field(f2.at(sizeIdx)) : 0;
        nd.alloc = allocIdx >= 0 && allocIdx < f2.size() ? parseU64Field(f2.at(allocIdx)) : nd.size;
        nd.mtime = mtimeIdx >= 0 && mtimeIdx < f2.size() ? parseTimeField(f2.at(mtimeIdx)) : 0;
        nd.attrs = attrIdx >= 0 && attrIdx < f2.size() ? parseAttrField(f2.at(attrIdx)) : 0;
        if (nd.isDir) {
            // Directory sizes are recomputed by finalize(); CSV values are already aggregated.
            nd.size = 0;
            nd.alloc = 0;
        }
        nd.nameOff = addName(name);
        nd.nameLen = uint32_t(name.size());
        tree.nodes.push_back(nd);
        if (looksDir) dirMap.insert(fullKey, uint32_t(tree.nodes.size() - 1));
        ++rows;
    }

    if (rows == 0) {
        res.error = QStringLiteral("No rows found in %1").arg(path);
        return res;
    }
    if (rootPath.isEmpty()) rootPath = QFileInfo(path).completeBaseName();
    tree.nodes[0].nameOff = addName(rootPath);
    tree.nodes[0].nameLen = uint32_t(rootPath.size());
    if (opt.status) opt.status(QStringLiteral("Building tree..."));
    tree.finalize(opt.cancel);
    res.ok = true;
    res.records = rows;
    res.seconds = timer.elapsed() / 1000.0;
    return res;
}

ScanResult scanDrive(wchar_t letter, const ScanOptions& opt)
{
    return runScanTask([&] {
        wchar_t root[8];
        swprintf_s(root, L"%c:\\", letter);
        if (opt.preferMft) {
            ScanResult r = scanNtfsImpl(letter, opt);
            if (r.ok || r.error == QLatin1String("canceled")) return r;
            checkCanceled(opt.cancel);
            // fall back to directory walk
        }
        ScanResult r = scanFolderImpl(QString::fromWCharArray(root), opt);
        if (!r.ok && r.error.isEmpty()) r.error = QStringLiteral("Scan failed");
        return r;
    });
}

ScanResult scanFolder(const QString& path, const ScanOptions& opt)
{
    return runScanTask([&] { return scanFolderImpl(path, opt); });
}

ScanResult scanMftDump(const QString& path, const ScanOptions& opt)
{
    return runScanTask([&] { return scanMftDumpImpl(path, opt); });
}

ScanResult importCsvTree(const QString& path, const ScanOptions& opt)
{
    return runScanTask([&] { return importCsvTreeImpl(path, opt); });
}

ScanResult dumpMftToFile(wchar_t letter, const QString& path, std::atomic_bool& cancel, const StatusFn& status)
{
    return runScanTask([&] { return dumpMftToFileImpl(letter, path, cancel, status); });
}

ScanResult runScanTask(const std::function<ScanResult()>& task)
{
    ScanResult result;
    try { return task(); }
    catch (const ScanCanceled&) { result.error = QStringLiteral("canceled"); }
    catch (const std::bad_alloc&) { result.error = QStringLiteral("Not enough memory to complete the operation"); }
    catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
    return result;
}


// ---------------------------------------------------------------- filtering

Tree filterTree(const Tree& src, const QString& include, const QString& exclude, bool matchPath)
{
    Tree out;
    if (src.nodes.empty() || src.root >= src.nodes.size()) return out;
    const uint32_t n = uint32_t(src.nodes.size());

    auto pathMatches = [&](uint32_t i, const QString& needle) {
        return src.path(i).contains(needle, Qt::CaseInsensitive);
    };

    std::vector<uint8_t> keep(n, 0);
    keep[src.root] = 1;
    for (uint32_t i = 0; i < n; ++i) {
        if (i == src.root || keep[i] || src.nodes[i].removed) continue;
        const QString name = src.name(i);
        bool hit = include.isEmpty() || name.contains(include, Qt::CaseInsensitive);
        if (!hit && matchPath && !include.isEmpty()) hit = pathMatches(i, include);
        if (!hit) continue;
        if (!exclude.isEmpty()) {
            if (name.contains(exclude, Qt::CaseInsensitive)) continue;
            if (matchPath && pathMatches(i, exclude)) continue;
        }
        uint32_t cur = i;
        int guard = 0;
        while (cur != kNone && cur < n && !keep[cur] && guard++ < 512) {
            keep[cur] = 1;
            cur = src.nodes[cur].parent;
        }
    }

    out.root = 0;
    out.nodes.reserve(size_t(n) / 4 + 16);
    out.names.reserve(src.names.size() / 4 + 64);

    auto copyName = [&](uint32_t oldIndex) {
        const Node& s = src.nodes[oldIndex];
        const uint32_t off = uint32_t(out.names.size());
        if (s.nameLen)
            out.names.insert(out.names.end(), src.names.data() + s.nameOff,
                             src.names.data() + s.nameOff + s.nameLen);
        return off;
    };

    {
        Node r = src.nodes[src.root];
        r.parent = kNone;
        r.size = 0;
        r.alloc = 0;
        r.files = 0;
        r.folders = 0;
        r.nameOff = copyName(src.root);
        out.nodes.push_back(r);
    }

    std::vector<uint32_t> map(n, kNone);
    map[src.root] = 0;
    std::vector<uint32_t> queue;
    queue.reserve(size_t(n) / 4 + 16);
    queue.push_back(src.root);

    for (size_t k = 0; k < queue.size(); ++k) {
        const uint32_t old = queue[k];
        const Node& pn = src.nodes[old];
        for (uint32_t j = 0; j < pn.childCount; ++j) {
            const uint32_t c = src.children[pn.childBegin + j];
            if (!keep[c]) continue;
            Node cn = src.nodes[c];
            cn.parent = map[old];
            if (cn.isDir) {
                cn.size = 0;
                cn.alloc = 0;
            }
            cn.files = 0;
            cn.folders = 0;
            cn.nameOff = copyName(c);
            map[c] = uint32_t(out.nodes.size());
            out.nodes.push_back(cn);
            queue.push_back(c);
        }
    }

    out.finalize();
    return out;
}
QString formatSize(uint64_t bytes, UnitMode mode, int decimals)
{
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = double(bytes);
    int u = 0;
    if (mode == UnitMode::Dynamic) {
        while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    } else {
        u = int(mode);
        for (int i = 0; i < u; ++i) v /= 1024.0;
    }
    if (u == 0) return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(v, 0, 'f', decimals).arg(QLatin1String(units[u]));
}

QString attributesText(uint32_t a)
{
    QString s;
    if (a & FILE_ATTRIBUTE_READONLY) s += QLatin1Char('R');
    if (a & FILE_ATTRIBUTE_HIDDEN) s += QLatin1Char('H');
    if (a & FILE_ATTRIBUTE_SYSTEM) s += QLatin1Char('S');
    if (a & FILE_ATTRIBUTE_ARCHIVE) s += QLatin1Char('A');
    if (a & FILE_ATTRIBUTE_COMPRESSED) s += QLatin1Char('C');
    if (a & FILE_ATTRIBUTE_ENCRYPTED) s += QLatin1Char('E');
    if (a & FILE_ATTRIBUTE_TEMPORARY) s += QLatin1Char('T');
    if (a & FILE_ATTRIBUTE_OFFLINE) s += QLatin1Char('O');
    return s;
}

bool moveToRecycleBin(const QStringList& paths)
{
    if (paths.isEmpty()) return false;
    std::wstring buf;
    for (const QString& p : paths) {
        buf += QDir::toNativeSeparators(p).toStdWString();
        buf.push_back(L'\0');
    }
    buf.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = buf.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0;
}

bool renameFileOrFolder(const QString& path, const QString& newName, QString& outNewPath,
                        QString& error)
{
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty()) {
        error = QStringLiteral("empty name");
        return false;
    }
    static const QString invalid = QStringLiteral("\\/:*?\"<>|");
    for (QChar ch : trimmed) {
        if (invalid.contains(ch)) {
            error = QStringLiteral("invalid character: %1").arg(ch);
            return false;
        }
    }
    const QFileInfo fi(path);
    const QString parent = fi.absolutePath();
    outNewPath = parent + QLatin1Char('/') + trimmed;
    if (QFileInfo::exists(outNewPath)) {
        error = QStringLiteral("target already exists");
        return false;
    }
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                     reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(outNewPath).utf16()),
                     MOVEFILE_COPY_ALLOWED)) {
        error = QStringLiteral("MoveFileEx failed (%1)").arg(GetLastError());
        return false;
    }
    return true;
}

bool isProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD len = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len) &&
              elev.TokenIsElevated;
    CloseHandle(token);
    return ok;
}


// ---------------------------------------------------------------- self test

bool selfTest(QString& out)
{
    QStringList log;
    bool ok = true;

    // 1) runlist: single run at LCN 0x1000 for 0x20 clusters
    {
        uint8_t rl[] = {0x21, 0x20, 0x00, 0x10, 0x00};  // len=1 byte, off=2 bytes -> LCN 0x1000
        std::vector<Run> runs;
        uint64_t ds = 0, as = 0;
        const bool parsed = parseRunList(rl, sizeof(rl), runs, ds, as);
        const bool good = parsed && runs.size() == 1 && runs[0].lcn == 0x1000 &&
                          runs[0].clusters == 0x20;
        log << QStringLiteral("runlist: %1 (lcn=0x%2 clusters=%3)")
                   .arg(good ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                   .arg(runs.empty() ? 0 : runs[0].lcn, 0, 16)
                   .arg(runs.empty() ? 0 : runs[0].clusters);
        ok = ok && good;
    }

    // 2) synthetic FILE record: fixups + STANDARD_INFORMATION + FILE_NAME
    {
        std::vector<uint8_t> rec(1024, 0);
        const uint32_t sectorSize = 512;
        memcpy(rec.data(), "FILE", 4);
        const uint16_t usn = 0x0001;
        const uint16_t usaOff = 0x30;
        const uint16_t usaCnt = 3;
        auto put16 = [&](size_t at, uint16_t v) { memcpy(rec.data() + at, &v, 2); };
        auto put32 = [&](size_t at, uint32_t v) { memcpy(rec.data() + at, &v, 4); };
        auto put64 = [&](size_t at, uint64_t v) { memcpy(rec.data() + at, &v, 8); };

        put16(0x04, usaOff);
        put16(0x06, usaCnt);
        put16(0x16, 0x0001);  // in use (file record)
        put32(0x1C, 1024);
        put32(0x2C, 42);
        put16(0x14, 0x38);  // attributes offset

        uint32_t off = 0x38;
        {
            const uint32_t len = 0x18 + 0x48;
            put32(off + 0x00, 0x10);
            put32(off + 0x04, len);
            put16(off + 0x10, 0x48);
            put16(off + 0x14, 0x18);
            put64(off + 0x18 + 0x08, 133000000000000000ull);
            put32(off + 0x18 + 0x20, 0x20);  // archive
            off += len;
        }
        {
            const char16_t nm[] = u"hello.txt";
            const uint32_t nameLen = 9;
            const uint32_t valueLen = 0x42 + nameLen * 2;
            const uint32_t len = (0x18 + valueLen + 7) & ~7u;
            put32(off + 0x00, 0x30);
            put32(off + 0x04, len);
            put16(off + 0x10, uint16_t(valueLen));
            put16(off + 0x14, 0x18);
            const size_t v = off + 0x18;
            put64(v + 0x00, 5);     // parent record 5
            put64(v + 0x28, 4096);  // allocated size
            put64(v + 0x30, 1234);  // real size
            rec[v + 0x40] = uint8_t(nameLen);
            rec[v + 0x41] = 1;  // Win32 namespace
            memcpy(rec.data() + v + 0x42, nm, nameLen * 2);
            off += len;
        }
        {
            // Non-resident unnamed $DATA: real/allocated must win over FILE_NAME sizes.
            const uint32_t len = 0x48;
            put32(off + 0x00, 0x80);
            put32(off + 0x04, len);
            rec[off + 0x08] = 1;   // non-resident
            rec[off + 0x09] = 0;   // unnamed
            put64(off + 0x10, 0);  // start VCN
            put64(off + 0x18, 0);  // last VCN
            put16(off + 0x20, 0x40);
            put64(off + 0x28, 12288);
            put64(off + 0x30, 9999);
            put64(off + 0x38, 9999);
            const uint8_t runs[] = {0x11, 0x01, 0x00, 0x00};
            memcpy(rec.data() + off + 0x40, runs, sizeof(runs));
            off += len;
        }
        put32(off, 0xFFFFFFFFu);
        put32(0x18, off + 4);  // used size

        // update-sequence array: USN then one replacement per sector
        put16(usaOff, usn);
        put16(usaOff + 2, 0x1111);
        put16(usaOff + 4, 0x2222);
        put16(sectorSize - 2, usn);
        put16(2 * sectorSize - 2, usn);

        std::vector<char16_t> arena;
        Rec r;
        const bool parsed = parseRecord(rec.data(), 1024, sectorSize, arena, r);
        const QString nm = arena.empty() ? QString() : QString::fromUtf16(arena.data(), arena.size());
        const bool good = parsed && r.inUse && !r.isDir && r.recNo == 42 && r.parent == 5 &&
                          r.nameLen == 9 && nm == QLatin1String("hello.txt") && r.size == 9999 &&
                          r.alloc == 12288 && r.attrs == 0x20;
        log << QStringLiteral("record: %1 (name=%2 parent=%3 size=%4 attrs=0x%5)")
                   .arg(good ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                   .arg(nm)
                   .arg(r.parent)
                   .arg(qulonglong(r.size))
                   .arg(r.attrs, 0, 16);
        ok = ok && good;
    }

    out = log.join(QLatin1Char('\n'));
    return ok;
}
}  // namespace wt









