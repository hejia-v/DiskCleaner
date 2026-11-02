#include "core.h"
#include "liveupdate.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>

namespace wt {
QString livePathKey(const QString& path)
{
    QString key = QDir::fromNativeSeparators(path).toCaseFolded();
    while (key.endsWith('/') && key.size() > 1) key.chop(1);
    return key;
}
namespace {
QString pathKey(QString path)
{
    path = QDir::fromNativeSeparators(path).toCaseFolded();
    while (path.endsWith('/') && path.size() > 1) path.chop(1);
    return path;
}
}

std::vector<uint32_t> Tree::findPaths(const QStringList& paths) const
{
    std::vector<uint32_t> found;
    if (root >= nodes.size()) return found;
    std::vector<std::pair<QString, uint32_t>> roots;
    if (QDir::isAbsolutePath(QDir::fromNativeSeparators(name(root))))
        roots.emplace_back(pathKey(name(root)), root);
    else { // merged scans have a virtual root and absolute paths as its children
        for (uint32_t j = 0; j < nodes[root].childCount; ++j) {
            const auto child = children[nodes[root].childBegin + j];
            roots.emplace_back(pathKey(name(child)), child);
        }
    }
    // Reuse lazy sibling indexes across batches, so a frequently written file in
    // a wide directory does not force a fresh sibling walk on every notification.
    auto& directories = liveChildren;
    for (const auto& path : paths) {
        const QString key = pathKey(path);
        for (const auto& [prefix, start] : roots) {
            if (key != prefix && !key.startsWith(prefix + '/')) continue;
            uint32_t current = start;
            const auto parts = key.mid(prefix.size()).split('/', Qt::SkipEmptyParts);
            for (const auto& part : parts) {
                if (current == kNone || nodes[current].removed) { current = kNone; break; }
                auto it = directories.find(current);
                if (it == directories.end()) {
                    QHash<QString, uint32_t> index;
                    const auto& parent = nodes[current];
                    index.reserve(parent.childCount);
                    for (uint32_t j = 0; j < parent.childCount; ++j) {
                        const auto child = children[parent.childBegin + j];
                        index.insert(name(child).toCaseFolded(), child);
                    }
                    it = directories.insert(current, std::move(index));
                }
                current = it->value(part, kNone);
            }
            if (current != kNone && !nodes[current].removed) found.push_back(current);
        }
    }
    return found;
}

uint32_t Tree::survivingAncestor(uint32_t id) const
{
    if (id >= nodes.size()) return root;
    while (id != root && nodes[id].removed) id = nodes[id].parent;
    return id;
}

Removal Tree::removeNodes(const std::vector<uint32_t>& ids)
{
    Removal result;
    QSet<uint32_t> requested;
    for (auto id : ids) if (id < nodes.size() && !nodes[id].removed) requested.insert(id);
    QSet<uint32_t> parents, ancestors;
    for (auto id : requested) {
        bool covered = false;
        for (auto p = nodes[id].parent; p != kNone; p = nodes[p].parent)
            if (requested.contains(p)) { covered = true; break; }
        if (covered) continue; // parent + child notifications must never double-subtract
        const Node removed = nodes[id];
        if (removed.parent != kNone) parents.insert(removed.parent);
        for (auto p = removed.parent; p != kNone; p = nodes[p].parent) {
            auto& parent = nodes[p];
            parent.size -= removed.size;
            parent.alloc -= removed.alloc;
            parent.files -= removed.files + !removed.isDir;
            parent.folders -= removed.folders + removed.isDir;
            ancestors.insert(p);
        }
        std::vector<uint32_t> stack{id};
        while (!stack.empty()) {
            const auto next = stack.back();
            stack.pop_back();
            auto& node = nodes[next];
            for (uint32_t j = 0; j < node.childCount; ++j)
                stack.push_back(children[node.childBegin + j]);
            node.removed = 1;
            liveChildren.remove(next);
            result.nodes.push_back(next);
            node.childCount = 0;
        }
    }
    for (auto parent : parents) {
        auto& node = nodes[parent];
        uint32_t count = 0;
        for (uint32_t j = 0; j < node.childCount; ++j) {
            const auto child = children[node.childBegin + j];
            if (nodes[child].removed) {
                if (liveChildren.contains(parent)) liveChildren[parent].remove(name(child).toCaseFolded());
                continue;
            }
            children[node.childBegin + count] = child;
            nodes[child].rowInParent = count++;
        }
        node.childCount = count;
    }
    // Keep the scan root as an empty placeholder when the whole target disappears.
    if (root < nodes.size() && nodes[root].removed) {
        nodes[root].size = nodes[root].alloc = 0;
        nodes[root].files = nodes[root].folders = 0;
    }
    result.ancestors.assign(ancestors.begin(), ancestors.end());
    return result;
}
} // namespace wt

namespace wt {
namespace {
// Lazy lookup shared by live batches, maintained as membership/names change.
class LiveLookup {
public:
    Tree& tree;
    QHash<uint32_t, QHash<QString, uint32_t>>& directories;
    explicit LiveLookup(Tree& t) : tree(t), directories(t.liveChildren) {}
    std::vector<uint32_t> roots() const {
        if (QDir::isAbsolutePath(QDir::fromNativeSeparators(tree.name(tree.root)))) return {tree.root};
        const auto& root = tree.nodes[tree.root];
        return {tree.children.begin() + root.childBegin, tree.children.begin() + root.childBegin + root.childCount};
    }
    uint32_t find(const QString& path, uint32_t root, bool includeRemovedRoot = false) {
        const auto key = livePathKey(path), prefix = livePathKey(tree.name(root));
        if (key != prefix && !key.startsWith(prefix + '/')) return kNone;
        auto current = root;
        for (const auto& part : key.mid(prefix.size()).split('/', Qt::SkipEmptyParts)) {
            if (tree.nodes[current].removed) return kNone;
            auto it = directories.find(current);
            if (it == directories.end()) {
                QHash<QString, uint32_t> index;
                const auto& parent = tree.nodes[current];
                index.reserve(parent.childCount);
                for (uint32_t j = 0; j < parent.childCount; ++j) {
                    const auto id = tree.children[parent.childBegin + j];
                    if (!tree.nodes[id].removed && tree.nodes[id].parent == current)
                        index.insert(tree.name(id).toCaseFolded(), id);
                }
                it = directories.insert(current, std::move(index));
            }
            current = it->value(part, kNone);
            if (current == kNone) return kNone;
        }
        return tree.nodes[current].removed && !(includeRemovedRoot && current == root) ? kNone : current;
    }
    void erase(uint32_t parent, const QString& name) {
        if (directories.contains(parent)) directories[parent].remove(name.toCaseFolded());
    }
    void insert(uint32_t parent, const QString& name, uint32_t id) {
        if (directories.contains(parent)) directories[parent].insert(name.toCaseFolded(), id);
    }
};
}

LiveDelta applyLiveUpdate(Tree& tree, const LiveUpdate& update, bool allowInsert)
{
    LiveDelta delta;
    if (tree.root >= tree.nodes.size()) return delta;
    LiveLookup lookup(tree);
    const auto roots = lookup.roots();
    QSet<uint32_t> dirty, ancestors;
    const auto rememberFile = [&](uint32_t id) {
        if (tree.nodes[id].isDir || delta.files.contains(id)) return;
        FileBefore before{tree.name(id), tree.nodes[id].size, tree.nodes[id].alloc, {}};
        for (auto p = id; p != kNone; p = tree.nodes[p].parent) before.ancestors.push_back(p);
        delta.files.insert(id, std::move(before));
    };
    const auto rememberSubtree = [&](uint32_t id) {
        std::vector<uint32_t> stack{id};
        while (!stack.empty()) {
            const auto next = stack.back(); stack.pop_back();
            if (tree.nodes[next].removed) continue;
            rememberFile(next);
            delta.changed.insert(next);
            const auto& node = tree.nodes[next];
            for (uint32_t j = 0; j < node.childCount; ++j) {
                const auto child = tree.children[node.childBegin + j];
                if (tree.nodes[child].parent == next) stack.push_back(child);
            }
        }
    };
    const auto adjust = [&](uint32_t parent, const Node& value, bool add) {
        for (auto p = parent; p != kNone; p = tree.nodes[p].parent) {
            auto& node = tree.nodes[p];
            if (add) {
                node.size += value.size; node.alloc += value.alloc;
                node.files += value.files + !value.isDir; node.folders += value.folders + value.isDir;
            } else {
                node.size -= value.size; node.alloc -= value.alloc;
                node.files -= value.files + !value.isDir; node.folders -= value.folders + value.isDir;
            }
            ancestors.insert(p);
        }
    };
    const auto nameNode = [&](uint32_t id, const QString& name) {
        if (tree.name(id) == name) return;
        auto& node = tree.nodes[id];
        node.nameOff = uint32_t(tree.names.size()); node.nameLen = uint32_t(name.size());
        const auto* chars = reinterpret_cast<const char16_t*>(name.utf16());
        tree.names.insert(tree.names.end(), chars, chars + name.size());
    };
    const auto attach = [&](uint32_t parent, uint32_t id) {
        auto& node = tree.nodes[parent];
        if (node.childCount == node.childCapacity) {
            const auto begin = uint32_t(tree.children.size());
            const auto capacity = std::max(uint32_t(4), node.childCapacity * 2);
            tree.children.resize(size_t(begin) + capacity);
            std::copy_n(tree.children.begin() + node.childBegin, node.childCount, tree.children.begin() + begin);
            node.childBegin = begin; node.childCapacity = capacity;
        }
        tree.children[node.childBegin + node.childCount] = id;
        tree.nodes[id].rowInParent = node.childCount++;
        lookup.insert(parent, tree.name(id), id);
        dirty.insert(parent);
    };
    const auto remove = [&](uint32_t id) {
        if (tree.nodes[id].removed) return;
        rememberSubtree(id);
        const Node before = tree.nodes[id];
        lookup.erase(before.parent, tree.name(id));
        adjust(before.parent, before, false);
        if (before.parent != kNone) dirty.insert(before.parent);
        std::vector<uint32_t> stack{id};
        while (!stack.empty()) {
            const auto next = stack.back(); stack.pop_back();
            auto& node = tree.nodes[next];
            for (uint32_t j = 0; j < node.childCount; ++j) {
                const auto child = tree.children[node.childBegin + j];
                if (tree.nodes[child].parent == next && !tree.nodes[child].removed) stack.push_back(child);
            }
            node.removed = 1; node.childCount = 0;
            delta.removal.nodes.push_back(next);
            lookup.directories.remove(next);
        }
        if (id == tree.root) {
            tree.nodes[id].size = tree.nodes[id].alloc = 0;
            tree.nodes[id].files = tree.nodes[id].folders = 0;
        }
        delta.structural = true;
    };

    std::function<uint32_t(const QString&, const QString&, const Node&, uint32_t)> upsert;
    upsert = [&](const QString& path, const QString& name, const Node& metadata, uint32_t root) -> uint32_t {
        auto id = lookup.find(path, root, true);
        if (id != kNone && !tree.nodes[id].removed && tree.nodes[id].isDir != metadata.isDir) { remove(id); id = kNone; }
        if (id == kNone || tree.nodes[id].removed) {
            if (!allowInsert) return kNone;
            const auto parent = id == root ? tree.nodes[id].parent : lookup.find(QFileInfo(path).absolutePath(), root);
            if (id != root && parent == kNone) return kNone;
            Node node = metadata;
            node.parent = parent; node.childBegin = node.childCount = node.childCapacity = 0;
            node.files = node.folders = 0; node.removed = 0; node.nameLen = 0;
            if (node.isDir) node.size = node.alloc = 0;
            if (id == root) tree.nodes[id] = node;
            else { id = uint32_t(tree.nodes.size()); tree.nodes.push_back(node); }
            nameNode(id, id == root ? QDir::toNativeSeparators(path) : name);
            if (parent != kNone) { attach(parent, id); adjust(parent, tree.nodes[id], true); }
            if (!node.isDir) delta.files.insert(id, {});
            delta.changed.insert(id); delta.structural = true;
        } else {
            auto& node = tree.nodes[id];
            const bool renamed = id != root && tree.name(id) != name;
            const bool changed = renamed || node.mtime != metadata.mtime || node.ctime != metadata.ctime || node.attrs != metadata.attrs ||
                (!node.isDir && (node.size != metadata.size || node.alloc != metadata.alloc));
            if (!changed) return id;
            rememberFile(id);
            if (!node.isDir) { adjust(node.parent, node, false); node.size = metadata.size; node.alloc = metadata.alloc; adjust(node.parent, node, true); }
            node.mtime = metadata.mtime; node.ctime = metadata.ctime; node.atime = metadata.atime; node.attrs = metadata.attrs;
            if (renamed) { lookup.erase(node.parent, tree.name(id)); nameNode(id, name); lookup.insert(node.parent, name, id); delta.structural = true; }
            delta.changed.insert(id);
            if (node.parent != kNone) ancestors.insert(node.parent);
        }
        return id;
    };

    QSet<QString> renameDestinations;
    for (const auto& rename : update.renames) renameDestinations.insert(livePathKey(rename.second));
    // A rename may target a directory created in this same batch. Materialize its
    // parent chain before moving existing IDs; postpone subtree import until afterwards.
    for (const auto& entry : update.entries)
        if (entry.state == LiveEntry::Present && entry.metadata.isDir && !renameDestinations.contains(livePathKey(entry.path)))
            for (auto root : roots) {
                const auto prefix = livePathKey(tree.name(root)), key = livePathKey(entry.path);
                if (key == prefix || key.startsWith(prefix + '/')) upsert(entry.path, entry.name, entry.metadata, root);
            }
    for (const auto& rename : update.renames) {
        for (auto root : roots) {
            const auto rootKey = livePathKey(tree.name(root)), oldKey = livePathKey(rename.first);
            if (rootKey.startsWith(oldKey + '/')) {
                // Overlapping scan targets may include both a renamed folder and a descendant.
                rememberSubtree(root);
                nameNode(root, QDir::toNativeSeparators(rename.second) + tree.name(root).mid(QDir::toNativeSeparators(rename.first).size()));
                delta.structural = true;
                delta.renames.append(rename);
                continue;
            }
            const auto id = lookup.find(rename.first, root);
            if (id == kNone) continue;
            const auto parent = id == root ? tree.nodes[id].parent
                : lookup.find(QFileInfo(rename.second).absolutePath(), root);
            if (id != root && parent == kNone) continue;
            bool cycle = false;
            for (auto p = parent; p != kNone; p = tree.nodes[p].parent) if (p == id) { cycle = true; break; }
            if (cycle) continue;
            const auto existing = lookup.find(rename.second, root);
            if (existing != kNone && existing != id) remove(existing);
            rememberSubtree(id);
            const auto oldParent = tree.nodes[id].parent;
            lookup.erase(oldParent, tree.name(id));
            nameNode(id, id == root ? QDir::toNativeSeparators(rename.second) : QFileInfo(rename.second).fileName());
            if (oldParent != parent) {
                adjust(oldParent, tree.nodes[id], false);
                tree.nodes[id].parent = parent;
                adjust(parent, tree.nodes[id], true);
                dirty.insert(oldParent);
                attach(parent, id);
            } else lookup.insert(parent, tree.name(id), id);
            if (parent != kNone) ancestors.insert(parent);
            delta.renames.append(rename);
            delta.structural = true;
        }
    }

    for (const auto& entry : update.entries) {
        if (entry.state == LiveEntry::Unavailable) continue;
        for (auto root : roots) {
            const auto prefix = livePathKey(tree.name(root)), key = livePathKey(entry.path);
            if (key != prefix && !key.startsWith(prefix + '/')) continue;
            if (entry.state == LiveEntry::Missing) {
                const auto id = lookup.find(entry.path, root);
                if (id != kNone) remove(id);
                continue;
            }
            const auto id = upsert(entry.path, entry.name, entry.metadata, root);
            if (id == kNone || entry.subtree.root == kNone) continue;
            QSet<uint32_t> present{id};
            // Import only the new subtree. Parent-first traversal also covers a moved-in tree.
            std::vector<std::pair<uint32_t, QString>> queue{{entry.subtree.root, entry.path}};
            for (size_t i = 0; i < queue.size(); ++i) {
                const auto [source, path] = queue[i];
                const auto& node = entry.subtree.nodes[source];
                for (uint32_t j = 0; j < node.childCount; ++j) {
                    const auto child = entry.subtree.children[node.childBegin + j];
                    const auto name = entry.subtree.name(child);
                    const auto childPath = QDir(path).filePath(name);
                    const auto imported = upsert(childPath, name, entry.subtree.nodes[child], root);
                    if (imported != kNone) {
                        present.insert(imported);
                        queue.emplace_back(child, childPath);
                    }
                }
            }
            // An added directory may replace an old directory at the same path within
            // one notification batch. Reconcile this subtree, not the entire scan.
            std::vector<uint32_t> stack{id};
            while (!stack.empty()) {
                const auto parent = stack.back(); stack.pop_back();
                const auto node = tree.nodes[parent];
                for (uint32_t j = 0; j < node.childCount; ++j) {
                    const auto child = tree.children[node.childBegin + j];
                    if (tree.nodes[child].removed || tree.nodes[child].parent != parent) continue;
                    if (!present.contains(child)) remove(child);
                    else if (tree.nodes[child].isDir) stack.push_back(child);
                }
            }
        }
    }
    for (auto parent : dirty) {
        if (parent == kNone) continue;
        auto& node = tree.nodes[parent];
        uint32_t count = 0;
        QSet<uint32_t> seen;
        for (uint32_t j = 0; j < node.childCount; ++j) {
            const auto child = tree.children[node.childBegin + j];
            if (tree.nodes[child].removed || tree.nodes[child].parent != parent || seen.contains(child)) continue;
            seen.insert(child);
            tree.children[node.childBegin + count] = child;
            tree.nodes[child].rowInParent = count++;
        }
        node.childCount = count;
        ancestors.insert(parent);
    }
    delta.removal.ancestors.assign(ancestors.begin(), ancestors.end());
    return delta;
}
}
