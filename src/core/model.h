// Model (IR): functions, call sites and control-flow regions with source ranges.
// Independent of any particular parser.
#pragma once

#include <string>
#include <vector>

namespace tracespace {

// Lines are 1-based, columns 0-based (bytes), matching nvim_win_get_cursor.
struct SrcRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
    bool contains(int l, int c) const {
        if (l < line || l > end_line) return false;
        if (l == line && c < col) return false;
        if (l == end_line && c >= end_col) return false;
        return true;
    }
    bool operator==(const SrcRange&) const = default;
};

enum class RegionKind {
    Call,          // direct call, resolved by name (target == -1: not defined in this file)
    IndirectCall,  // function pointer / virtual call
    Loop,
    If,            // children are Lanes
    Switch,        // children are Lanes
    Lane,          // one branch of an If or Switch
    Return,
    Break,
    Continue,
    Goto,
    Label,
};

struct Region {
    RegionKind kind = RegionKind::Call;
    SrcRange range;
    // Call: callee name. IndirectCall: callee expression. Lane: label ("if (x > 0)",
    // "else", "case 1", "default"). Switch: condition. Goto/Label: label name.
    std::string name;
    std::string qualifier;                // Call: explicit scope ("Foo" in Foo::bar())
    std::string callee;                   // IndirectCall: virtual method name, if any
    int target = -1;                      // Call: index into Model::functions, or -1
    std::string loop_kind;                // Loop: for | while | do | range-for
    std::string count;                    // Loop: "<N>", "?" or "inf"
    std::vector<std::string> candidates;  // IndirectCall: possible targets found by name
    std::vector<Region> children;
};

struct Function {
    std::string name;       // last component, e.g. "bar"
    std::string qualified;  // e.g. "Foo::bar" (same as name for free functions)
    std::string scope;      // enclosing class, or ""
    SrcRange range;
    std::vector<Region> body;
    bool has_error = false;  // an ERROR node lies inside this function
    bool stale = false;      // carried over from the last good parse
};

struct Model {
    std::string path;
    bool is_cpp = true;
    std::vector<Function> functions;
    std::vector<SrcRange> errors;  // outermost parse-error ranges

    int find(const std::string& name) const;  // first function with this name, or -1
    // Roots for execution order: main, else functions nobody else calls (source order).
    std::vector<int> entry_points() const;
    // Innermost function containing the position, or -1.
    int function_at(int line, int col) const;
};

// Fill in Call::target and IndirectCall::candidates by name. Idempotent.
void resolve_calls(Model& m);

// Keep the last good version of functions the fresh parse lost or could not read.
Model merge_last_good(Model fresh, const Model& last_good);

// Indented text form of the IR (used by --dump and the tests).
std::string dump(const Model& m, bool positions = true);

}  // namespace tracespace
