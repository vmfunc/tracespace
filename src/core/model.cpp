#include "core/model.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace tracespace {

int Model::find(const std::string& name) const {
    for (size_t i = 0; i < functions.size(); ++i)
        if (functions[i].name == name || functions[i].qualified == name) return (int)i;
    return -1;
}

int Model::function_at(int line, int col) const {
    int best = -1;
    for (size_t i = 0; i < functions.size(); ++i) {
        const auto& f = functions[i];
        if (f.stale || !f.range.contains(line, col)) continue;
        if (best < 0 || functions[best].range.contains(f.range.line, f.range.col)) best = (int)i;
    }
    return best;
}

static void for_each_region(const std::vector<Region>& rs,
                            const std::function<void(const Region&)>& fn) {
    for (const auto& r : rs) {
        fn(r);
        for_each_region(r.children, fn);
    }
}

std::vector<int> Model::entry_points() const {
    int m = -1;
    for (size_t i = 0; i < functions.size(); ++i)
        if (functions[i].qualified == "main") m = (int)i;
    if (m >= 0) return {m};

    std::vector<bool> called(functions.size(), false);
    for (size_t i = 0; i < functions.size(); ++i) {
        for_each_region(functions[i].body, [&](const Region& r) {
            if (r.kind == RegionKind::Call && r.target >= 0 && r.target != (int)i)
                called[r.target] = true;
        });
    }
    std::vector<int> roots;
    for (size_t i = 0; i < functions.size(); ++i)
        if (!called[i]) roots.push_back((int)i);
    if (roots.empty() && !functions.empty()) roots.push_back(0);  // everything is in a cycle
    return roots;
}

void resolve_calls(Model& m) {
    std::map<std::string, std::vector<int>> by_name;
    for (size_t i = 0; i < m.functions.size(); ++i) by_name[m.functions[i].name].push_back((int)i);

    auto lookup = [&](const Region& r, const std::string& scope) {
        auto it = by_name.find(r.name);
        if (it == by_name.end()) return -1;
        const auto& c = it->second;
        auto match = [&](const std::string& q) {
            for (int i : c) {
                const std::string& fq = m.functions[i].qualified;
                if (fq == q) return i;
                if (fq.size() > q.size() && fq.compare(fq.size() - q.size(), q.size(), q) == 0 &&
                    fq[fq.size() - q.size() - 1] == ':')
                    return i;
            }
            return -1;
        };
        int hit = -1;
        if (!r.qualifier.empty()) hit = match(r.qualifier + "::" + r.name);
        if (hit < 0 && !scope.empty()) hit = match(scope + "::" + r.name);
        if (hit >= 0) return hit;
        // An unqualified call prefers a free function over some class's method.
        if (r.qualifier.empty())
            for (int i : c)
                if (m.functions[i].scope.empty()) return i;
        return c.front();
    };

    std::function<void(std::vector<Region>&, const std::string&)> walk =
        [&](std::vector<Region>& rs, const std::string& scope) {
            for (auto& r : rs) {
                if (r.kind == RegionKind::Call) {
                    r.target = lookup(r, scope);
                } else if (r.kind == RegionKind::IndirectCall) {
                    std::vector<std::string> out;
                    if (!r.callee.empty()) {
                        auto it = by_name.find(r.callee);
                        if (it != by_name.end())
                            for (int i : it->second) out.push_back(m.functions[i].qualified);
                    } else {
                        for (const auto& c : r.candidates)
                            if (m.find(c) >= 0 && std::find(out.begin(), out.end(), c) == out.end())
                                out.push_back(c);
                    }
                    r.candidates = std::move(out);
                }
                walk(r.children, scope);
            }
        };
    for (auto& f : m.functions) walk(f.body, f.scope);
}

Model merge_last_good(Model fresh, const Model& last_good) {
    if (fresh.errors.empty()) return fresh;  // clean parse: nothing to preserve
    bool changed = false;

    // A function the parser could not make sense of keeps its last good body.
    for (auto& f : fresh.functions) {
        if (!f.has_error || !f.body.empty()) continue;
        for (const auto& old : last_good.functions) {
            if (old.qualified == f.qualified && !old.body.empty()) {
                f.body = old.body;
                f.stale = true;
                changed = true;
                break;
            }
        }
    }
    // A function that vanished while the file is broken is carried over.
    std::set<std::string> have;
    for (const auto& f : fresh.functions) have.insert(f.qualified);
    for (size_t i = 0; i < last_good.functions.size(); ++i) {
        const auto& old = last_good.functions[i];
        if (have.count(old.qualified)) continue;
        Function keep = old;
        keep.stale = true;
        keep.has_error = true;
        size_t at = std::min(i, fresh.functions.size());
        fresh.functions.insert(fresh.functions.begin() + (long long)at, std::move(keep));
        have.insert(old.qualified);
        changed = true;
    }
    if (changed) resolve_calls(fresh);
    return fresh;
}

// ---------------------------------------------------------------- dump

static std::string pos(const SrcRange& r) {
    std::ostringstream o;
    o << " @" << r.line << ":" << r.col << "-" << r.end_line << ":" << r.end_col;
    return o.str();
}

static void dump_regions(std::ostringstream& o, const Model& m, const std::vector<Region>& rs,
                         int depth, bool positions) {
    for (const auto& r : rs) {
        o << std::string((size_t)depth * 2, ' ');
        switch (r.kind) {
            case RegionKind::Call:
                o << "call " << (r.qualifier.empty() ? "" : r.qualifier + "::") << r.name;
                if (r.target < 0) o << " [lib]";
                else if (m.functions[r.target].qualified != r.name)
                    o << " -> " << m.functions[r.target].qualified;
                break;
            case RegionKind::IndirectCall:
                o << "indirect ?(" << r.name << ")";
                if (!r.candidates.empty()) {
                    o << " -> {";
                    for (size_t i = 0; i < r.candidates.size(); ++i)
                        o << (i ? ", " : "") << r.candidates[i];
                    o << "}";
                }
                break;
            case RegionKind::Loop: o << "loop " << r.loop_kind << " x" << r.count; break;
            case RegionKind::If: o << "if"; break;
            case RegionKind::Switch: o << "switch " << r.name; break;
            case RegionKind::Lane: o << "lane " << r.name; break;
            case RegionKind::Return: o << "return"; break;
            case RegionKind::Break: o << "break"; break;
            case RegionKind::Continue: o << "continue"; break;
            case RegionKind::Goto: o << "goto " << r.name; break;
            case RegionKind::Label: o << "label " << r.name; break;
        }
        if (positions) o << pos(r.range);
        o << "\n";
        dump_regions(o, m, r.children, depth + 1, positions);
    }
}

std::string dump(const Model& m, bool positions) {
    std::ostringstream o;
    if (positions)
        o << "file " << m.path << " [" << (m.is_cpp ? "cpp" : "c") << "] functions="
          << m.functions.size() << " errors=" << m.errors.size() << "\n";
    for (const auto& f : m.functions) {
        o << "fn " << f.qualified;
        if (f.has_error) o << " [error]";
        if (f.stale) o << " [stale]";
        if (positions) o << pos(f.range);
        o << "\n";
        dump_regions(o, m, f.body, 1, positions);
    }
    if (positions)
        for (const auto& e : m.errors) o << "error" << pos(e) << "\n";
    return o.str();
}

}  // namespace tracespace
