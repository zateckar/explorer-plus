#include "sync_plan.h"
#include <algorithm>
#include <set>

namespace sync {

const char* act_name(Act a) {
    switch (a) {
        case Act::Upload: return "upload";
        case Act::Download: return "download";
        case Act::Conflict: return "conflict";
        case Act::DeleteRemote: return "delete-remote";
        case Act::DeleteLocal: return "delete-local";
        case Act::MoveRemote: return "move-remote";
        case Act::MoveLocal: return "move-local";
        case Act::MkdirLocal: return "mkdir-local";
        case Act::MkdirRemote: return "mkdir-remote";
        case Act::RmdirLocal: return "rmdir-local";
        case Act::RmdirRemote: return "rmdir-remote";
        case Act::Adopt: return "adopt";
        case Act::Refresh: return "refresh";
        case Act::Forget: return "forget";
        case Act::TypeClash: return "type-clash";
    }
    return "?";
}

void add_implied_dirs(RemoteMap& remote) {
    std::vector<RemoteItem> add;
    std::set<std::wstring> seen;
    for (auto& [k, r] : remote) {
        RelPath p = parent_of(r.path);
        while (!p.empty()) {
            std::wstring pk = key_of(p);
            if (remote.count(pk) || !seen.insert(pk).second) break;
            RemoteItem d;
            d.path = p;
            d.isDir = true;
            add.push_back(d);
            p = parent_of(p);
        }
    }
    for (auto& d : add) remote[key_of(d.path)] = d;
}

static bool local_changed(const LocalItem& l, const BaseEntry& b) {
    if (l.isDir != b.isDir) return true;
    if (l.isDir) return false;
    if (!l.racy && l.size == b.size && l.mtime == b.mtime && l.fileId == b.fileId) return false;
    return !l.hashed || l.hash != b.hash;
}
static bool local_meta_changed(const LocalItem& l, const BaseEntry& b) {
    return !l.isDir && (l.size != b.size || l.mtime != b.mtime || l.fileId != b.fileId);
}
static bool remote_changed(const RemoteItem& r, const BaseEntry& b) {
    if (r.isDir != b.isDir) return true;
    return !r.isDir && r.rev != b.rev;
}

Plan make_plan(const BaseMap& base, const LocalMap& local, const RemoteMap& remote, const Caps& caps) {
    Plan plan;
    std::set<std::wstring> keys;
    for (auto& [k, v] : base) { keys.insert(k); if (!v.isDir) ++plan.files; }
    for (auto& [k, v] : local) keys.insert(k);
    for (auto& [k, v] : remote) keys.insert(k);

    // a busy local item holds back its path and the folders above it (no deletion of a
    // folder with a file being written)
    std::set<std::wstring> busyDirs;
    for (auto& [k, l] : local)
        if (l.busy) {
            plan.deferred = true;
            for (RelPath p = parent_of(l.path); !p.empty(); p = parent_of(p)) busyDirs.insert(key_of(p));
        }

    for (const std::wstring& k : keys) {
        auto bi = base.find(k);
        auto li = local.find(k);
        auto ri = remote.find(k);
        const BaseEntry* b = bi == base.end() ? nullptr : &bi->second;
        const LocalItem* l = li == local.end() ? nullptr : &li->second;
        const RemoteItem* r = ri == remote.end() ? nullptr : &ri->second;
        if (l && l->busy) continue;
        const RelPath& path = l ? l->path : r ? r->path : b->path;
        auto add = [&](Act a, std::string rev = {}) { plan.actions.push_back({ a, path, {}, std::move(rev) }); };

        // ---- folders (and items that are a folder on one side)
        bool anyDir = (b && b->isDir) || (l && l->isDir) || (r && r->isDir);
        if (anyDir) {
            if (l && r && l->isDir != r->isDir) { add(Act::TypeClash); continue; }
            if (b && b->isDir) {
                if (l && !l->isDir) {                  // replaced by a file locally
                    if (r) { add(Act::TypeClash); continue; }
                    add(Act::Upload, ""); continue;
                }
                if (r && !r->isDir) {                  // replaced by a file in the store
                    if (l) { add(Act::TypeClash); continue; }
                    add(Act::Download); continue;
                }
                if (l && r) continue;                  // both still there
                if (l && !r) { if (!busyDirs.count(k)) add(Act::RmdirLocal); continue; }
                if (!l && r) {
                    if (caps.emptyDirs) add(Act::RmdirRemote);         // a folder the store only implies: NotFound is fine
                    else add(Act::Forget);
                    continue;
                }
                add(Act::Forget);
                continue;
            }
            if (b && !b->isDir) {                      // was a file, a folder now on one side
                bool lDir = l && l->isDir, rDir = r && r->isDir;
                if (lDir && !r) { add(Act::DeleteRemote, b->rev); ++plan.remoteDeletes; continue; }   // its folder: next pass
                if (rDir && !l) { add(Act::Forget); continue; }
                add(Act::TypeClash);
                continue;
            }
            // no base
            if (l && r) { add(Act::Adopt); continue; }
            if (l) { if (caps.emptyDirs) add(Act::MkdirRemote); continue; }
            if (r) { add(Act::MkdirLocal); continue; }
            continue;
        }

        // ---- files
        if (b) {
            bool lc = l && local_changed(*l, *b), rc = r && remote_changed(*r, *b);
            bool ld = !l, rd = !r;
            if (ld && rd) { add(Act::Forget); continue; }
            if (!ld && !rd) {
                if (lc && rc) { add(Act::Conflict); continue; }
                if (lc) { add(Act::Upload, b->rev); continue; }
                if (rc) { add(Act::Download); continue; }
                if (local_meta_changed(*l, *b)) add(Act::Refresh);
                continue;
            }
            if (ld) {                                  // deleted here
                if (rc) add(Act::Download);            // an edit beats a delete
                else { add(Act::DeleteRemote, b->rev); ++plan.remoteDeletes; }
                continue;
            }
            // deleted in the store
            if (lc) add(Act::Upload, "");
            else { add(Act::DeleteLocal); ++plan.localDeletes; }
            continue;
        }
        if (l && r) { add(Act::Conflict); continue; }  // new on both sides: same content is adopted
        if (l) { add(Act::Upload, ""); continue; }
        if (r) { add(Act::Download); continue; }
    }

    // ---- renames: a delete and an add of the same file become a move
    // local renames (same file ID, same content) -> MoveRemote
    if (caps.serverMove) {
        std::unordered_map<uint64_t, size_t> delById;          // fileId of a base file whose path is gone locally
        for (size_t i = 0; i < plan.actions.size(); ++i) {
            const Action& a = plan.actions[i];
            if (a.act != Act::DeleteRemote) continue;
            auto b = base.find(key_of(a.path));
            if (b != base.end() && b->second.fileId) delById[b->second.fileId] = i;
        }
        for (auto& a : plan.actions) {
            if (a.act != Act::Upload || !a.expectedRev.empty()) continue;
            auto l = local.find(key_of(a.path));
            if (l == local.end() || !l->second.fileId || base.count(key_of(a.path))) continue;
            auto d = delById.find(l->second.fileId);
            if (d == delById.end()) continue;
            Action& del = plan.actions[d->second];
            auto b = base.find(key_of(del.path));
            if (b == base.end() || !l->second.hashed || l->second.hash != b->second.hash) continue;
            a.act = Act::MoveRemote;
            a.from = del.path;
            a.expectedRev = del.expectedRev;
            del.path.clear();                                  // the move takes care of it (dropped below)
            --plan.remoteDeletes;
            delById.erase(d);
        }
    }
    // store renames (same stored bytes) -> MoveLocal
    {
        std::unordered_map<std::string, size_t> delByRev;
        for (size_t i = 0; i < plan.actions.size(); ++i) {
            const Action& a = plan.actions[i];
            if (a.act != Act::DeleteLocal) continue;
            auto b = base.find(key_of(a.path));
            if (b != base.end() && !b->second.rev.empty()) delByRev[b->second.rev] = i;
        }
        for (auto& a : plan.actions) {
            if (a.act != Act::Download || base.count(key_of(a.path)) || local.count(key_of(a.path))) continue;
            auto r = remote.find(key_of(a.path));
            if (r == remote.end() || r->second.rev.empty()) continue;
            auto d = delByRev.find(r->second.rev);
            if (d == delByRev.end()) continue;
            Action& del = plan.actions[d->second];
            a.act = Act::MoveLocal;
            a.from = del.path;
            del.path.clear();
            --plan.localDeletes;
            delByRev.erase(d);
        }
    }
    std::erase_if(plan.actions, [](const Action& a) { return a.path.empty(); });
    order_actions(plan.actions);
    return plan;
}

static int depth(const RelPath& p) { return (int)std::count(p.begin(), p.end(), L'/'); }

void order_actions(std::vector<Action>& a) {
    auto rank = [](Act x) {
        switch (x) {
            case Act::Forget: case Act::Refresh: case Act::Adopt: return 0;
            case Act::MkdirLocal: return 1;
            case Act::TypeClash: return 2;
            case Act::MoveLocal: return 3;
            case Act::Conflict: return 4;
            case Act::Download: return 5;
            case Act::DeleteLocal: return 6;
            case Act::RmdirLocal: return 7;
            case Act::MkdirRemote: return 8;
            case Act::MoveRemote: return 9;
            case Act::Upload: return 10;
            case Act::DeleteRemote: return 11;
            case Act::RmdirRemote: return 12;
        }
        return 13;
    };
    std::stable_sort(a.begin(), a.end(), [&](const Action& x, const Action& y) {
        int rx = rank(x.act), ry = rank(y.act);
        if (rx != ry) return rx < ry;
        bool deep = x.act == Act::RmdirLocal || x.act == Act::RmdirRemote;
        int dx = depth(x.path), dy = depth(y.path);
        if (dx != dy) return deep ? dx > dy : dx < dy;
        return x.path < y.path;
    });
}

} // namespace sync
