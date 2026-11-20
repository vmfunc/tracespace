// tree-sitter implementation of the Parser interface.
#include <tree_sitter/api.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/parser.h"

extern "C" const TSLanguage* tree_sitter_c(void);
extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace tracespace {

bool path_is_cpp(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return true;
    std::string ext = path.substr(dot);
    return !(ext == ".c" || ext == ".h");
}

namespace {

bool is(TSNode n, const char* type) { return !ts_node_is_null(n) && !std::strcmp(ts_node_type(n), type); }
bool null(TSNode n) { return ts_node_is_null(n); }
TSNode field(TSNode n, const char* name) {
    return ts_node_child_by_field_name(n, name, (uint32_t)std::strlen(name));
}
TSNode named0(TSNode n) { return ts_node_named_child_count(n) ? ts_node_named_child(n, 0) : TSNode{}; }

SrcRange range_of(TSNode n) {
    TSPoint a = ts_node_start_point(n), b = ts_node_end_point(n);
    return {(int)a.row + 1, (int)a.column, (int)b.row + 1, (int)b.column};
}

struct Builder {
    std::string_view src;
    Model model;
    std::set<std::string> virtuals;  // names of virtual methods
    std::set<std::string> vars;      // file-scope variables and data members
    std::set<std::string> defined;   // names of functions defined in the file
    std::map<std::string, std::vector<std::string>> assigned;  // var -> names assigned to it
    struct Def {
        TSNode node;
        std::string scope;
    };
    std::vector<Def> defs;

    // Child iteration with cursors reused per recursion depth (a fresh cursor
    // per node costs an allocation each, which dominated the build time).
    mutable std::vector<TSTreeCursor> cursors;
    mutable size_t cursor_depth = 0;
    ~Builder() {
        for (auto& c : cursors) ts_tree_cursor_delete(&c);
    }
    template <class F>
    void each_child(TSNode n, F&& f) const {
        if (ts_node_child_count(n) == 0) return;
        size_t d = cursor_depth++;
        if (d == cursors.size()) cursors.push_back(ts_tree_cursor_new(n));
        else ts_tree_cursor_reset(&cursors[d], n);
        if (ts_tree_cursor_goto_first_child(&cursors[d])) {
            do {
                const char* fname = ts_tree_cursor_current_field_name(&cursors[d]);
                f(ts_tree_cursor_current_node(&cursors[d]), std::string_view(fname ? fname : ""));
            } while (ts_tree_cursor_goto_next_sibling(&cursors[d]));
        }
        --cursor_depth;
    }
    // Same, without the per-child field-name lookup.
    template <class F>
    void each_kid(TSNode n, F&& f) const {
        if (ts_node_child_count(n) == 0) return;
        size_t d = cursor_depth++;
        if (d == cursors.size()) cursors.push_back(ts_tree_cursor_new(n));
        else ts_tree_cursor_reset(&cursors[d], n);
        if (ts_tree_cursor_goto_first_child(&cursors[d])) {
            do {
                f(ts_tree_cursor_current_node(&cursors[d]));
            } while (ts_tree_cursor_goto_next_sibling(&cursors[d]));
        }
        --cursor_depth;
    }

    std::string raw(TSNode n) const {
        if (null(n)) return {};
        return std::string(src.substr(ts_node_start_byte(n), ts_node_end_byte(n) - ts_node_start_byte(n)));
    }
    // Source text with runs of whitespace collapsed to single spaces.
    std::string text(TSNode n) const {
        std::string s = raw(n), out;
        bool space = false;
        for (char ch : s) {
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                space = !out.empty();
            } else {
                if (space) out += ' ';
                space = false;
                out += ch;
            }
        }
        return out;
    }

    // The identifier a declarator declares. *plain_fn is set when the declarator
    // is an ordinary function declarator (a prototype, not a function pointer).
    TSNode decl_ident(TSNode d, bool* plain_fn) const {
        for (int guard = 0; guard < 32 && !null(d); ++guard) {
            const char* t = ts_node_type(d);
            if (!std::strcmp(t, "identifier") || !std::strcmp(t, "field_identifier")) return d;
            if (!std::strcmp(t, "function_declarator")) {
                TSNode inner = field(d, "declarator");
                if (!is(inner, "parenthesized_declarator") && plain_fn) *plain_fn = true;
                d = inner;
            } else if (!std::strcmp(t, "parenthesized_declarator") ||
                       !std::strcmp(t, "reference_declarator")) {
                d = named0(d);
            } else {
                d = field(d, "declarator");
            }
        }
        return TSNode{};
    }

    TSNode find_function_declarator(TSNode d) const {
        for (int guard = 0; guard < 32 && !null(d); ++guard) {
            if (is(d, "function_declarator")) return d;
            TSNode next = field(d, "declarator");
            d = null(next) ? named0(d) : next;
        }
        return TSNode{};
    }

    // Name of a function from its function_declarator: {last component, as written}.
    std::pair<std::string, std::string> function_name(TSNode fdecl) const {
        TSNode n = field(fdecl, "declarator");
        if (null(n) || is(n, "parenthesized_declarator")) return {};
        std::string full = text(n);
        if (is(n, "template_function")) full = text(field(n, "name"));
        std::string last = full;
        if (is(n, "qualified_identifier")) {
            TSNode leaf = n;
            while (is(leaf, "qualified_identifier")) leaf = field(leaf, "name");
            if (is(leaf, "template_function")) leaf = field(leaf, "name");
            last = text(leaf);
            // "Foo :: bar" -> "Foo::bar"
            std::string q;
            for (char ch : full)
                if (ch != ' ') q += ch;
            full = q;
        }
        return {last, full};
    }

    void collect_refs(TSNode v, std::vector<std::string>& out, int depth = 0) const {
        if (null(v) || depth > 4) return;
        if (is(v, "identifier")) {
            out.push_back(text(v));
        } else if (is(v, "pointer_expression") || is(v, "parenthesized_expression")) {
            TSNode a = field(v, "argument");
            collect_refs(null(a) ? named0(v) : a, out, depth + 1);
        } else if (is(v, "initializer_list")) {
            each_child(v, [&](TSNode c, std::string_view) {
                if (ts_node_is_named(c)) collect_refs(c, out, depth + 1);
            });
        } else if (is(v, "initializer_pair")) {
            collect_refs(field(v, "value"), out, depth + 1);
        }
    }

    // Variable name behind a callee / assignment target expression.
    std::string base_name(TSNode e) const {
        for (int guard = 0; guard < 16 && !null(e); ++guard) {
            if (is(e, "identifier") || is(e, "field_identifier")) return text(e);
            if (is(e, "field_expression")) e = field(e, "field");
            else if (is(e, "subscript_expression") || is(e, "pointer_expression")) e = field(e, "argument");
            else if (is(e, "parenthesized_expression")) e = named0(e);
            else break;
        }
        return {};
    }

    // tree-sitter-cpp reads `void (*name)()`, a function pointer with an empty
    // parameter list, as the expression `void(*name)()`. Returns the `name` node
    // if n has that shape (optionally followed by `= value`), else a null node.
    TSNode misparsed_fnptr(TSNode n) const {
        if (is(n, "assignment_expression")) n = field(n, "left");
        if (!is(n, "call_expression")) return TSNode{};
        TSNode inner = field(n, "function");
        if (!is(inner, "call_expression") || !is(field(inner, "function"), "primitive_type")) return TSNode{};
        TSNode args = field(inner, "arguments");
        if (null(args) || ts_node_named_child_count(args) != 1) return TSNode{};
        TSNode ptr = ts_node_named_child(args, 0);
        if (!is(ptr, "pointer_expression")) return TSNode{};
        TSNode id = field(ptr, "argument");
        return is(id, "identifier") ? id : TSNode{};
    }

    // Pass 1: find function definitions, virtual methods, variables and errors.
    void scan(TSNode n, const std::string* scope, bool in_function, bool in_error) {
        const char* t = ts_node_type(n);
        bool error = !std::strcmp(t, "ERROR") || ts_node_is_missing(n);
        if (error && !in_error) model.errors.push_back(range_of(n));
        in_error = in_error || error;

        std::string class_scope;
        if (!std::strcmp(t, "function_definition")) {
            defs.push_back({n, *scope});
            in_function = true;
        } else if (!std::strcmp(t, "class_specifier") || !std::strcmp(t, "struct_specifier")) {
            TSNode name = field(n, "name");
            if (!null(name) && !null(field(n, "body"))) {
                class_scope = text(name);
                scope = &class_scope;
            }
        } else if (!std::strcmp(t, "virtual") && !ts_node_is_named(n)) {
            TSNode fd = find_function_declarator(field(ts_node_parent(n), "declarator"));
            if (!null(fd)) virtuals.insert(function_name(fd).first);
        } else if (!std::strcmp(t, "virtual_specifier")) {
            TSNode p = ts_node_parent(n);
            if (is(p, "function_declarator")) virtuals.insert(function_name(p).first);
        } else if (!std::strcmp(t, "field_declaration") ||
                   (!std::strcmp(t, "declaration") && !in_function)) {
            each_child(n, [&](TSNode c, std::string_view f) {
                if (f != "declarator") return;
                bool plain = false;
                TSNode id = decl_ident(c, &plain);
                if (!null(id) && !plain) vars.insert(text(id));
            });
        } else if (!std::strcmp(t, "init_declarator")) {
            TSNode id = decl_ident(field(n, "declarator"), nullptr);
            if (!null(id)) collect_refs(field(n, "value"), assigned[text(id)]);
        } else if (!std::strcmp(t, "expression_statement")) {
            TSNode e = named0(n);
            TSNode id = misparsed_fnptr(e);
            if (!null(id)) {  // really a function-pointer declaration
                if (!in_function) vars.insert(text(id));
                if (is(e, "assignment_expression")) collect_refs(field(e, "right"), assigned[text(id)]);
            }
        } else if (!std::strcmp(t, "assignment_expression")) {
            std::string name = base_name(field(n, "left"));
            if (!name.empty()) collect_refs(field(n, "right"), assigned[name]);
        }
        each_kid(n, [&](TSNode c) { scan(c, scope, in_function, in_error); });
    }

    // ------------------------------------------------------------ pass 2

    struct FnCtx {
        std::set<std::string> locals;
        std::vector<char> breakable;  // 'L' loop, 'S' switch
        int lambda_depth = 0;         // exits inside a lambda body are not the function's
        bool has_error = false;       // the function contains a parse error
    };

    void collect_locals(TSNode n, FnCtx& fc, bool top) const {
        const char* t = ts_node_type(n);
        if (!top && (!std::strcmp(t, "function_definition") || !std::strcmp(t, "class_specifier") ||
                     !std::strcmp(t, "struct_specifier")))
            return;
        if (!std::strcmp(t, "parameter_declaration") ||
            !std::strcmp(t, "optional_parameter_declaration") || !std::strcmp(t, "declaration")) {
            each_child(n, [&](TSNode c, std::string_view f) {
                if (f != "declarator") return;
                bool plain = false;
                TSNode id = decl_ident(c, &plain);
                if (!null(id) && !plain) fc.locals.insert(text(id));
            });
        }
        if (!std::strcmp(t, "expression_statement")) {
            TSNode id = misparsed_fnptr(named0(n));
            if (!null(id)) fc.locals.insert(text(id));
        }
        each_kid(n, [&](TSNode c) { collect_locals(c, fc, false); });
    }

    static bool parse_int(const std::string& s, long long* out) {
        if (s.empty()) return false;
        char* end = nullptr;
        long long v = std::strtoll(s.c_str(), &end, 0);
        if (end == s.c_str()) return false;
        for (; *end; ++end)
            if (!std::strchr("uUlL", *end)) return false;
        *out = v;
        return true;
    }
    bool literal_int(TSNode n, long long* out) const {
        if (is(n, "number_literal")) return parse_int(text(n), out);
        if (is(n, "unary_expression") && text(field(n, "operator")) == "-") {
            long long v;
            if (!literal_int(field(n, "argument"), &v)) return false;
            *out = -v;
            return true;
        }
        return false;
    }
    // 1 = always true, 0 = always false, -1 = unknown
    int constant_truth(TSNode cond) const {
        if (is(cond, "condition_clause")) cond = field(cond, "value");
        while (is(cond, "parenthesized_expression")) cond = named0(cond);
        if (is(cond, "true")) return 1;
        if (is(cond, "false")) return 0;
        long long v;
        if (literal_int(cond, &v)) return v != 0;
        return -1;
    }

    std::string for_count(TSNode n) const {
        TSNode init = field(n, "initializer"), cond = field(n, "condition"), upd = field(n, "update");
        if (null(cond) || constant_truth(cond) == 1) return "inf";
        if (null(init) || null(upd) || !is(cond, "binary_expression")) return "?";

        std::string var;
        long long a = 0, b = 0, step = 0;
        if (is(init, "declaration")) {
            TSNode d = field(init, "declarator");
            if (!is(d, "init_declarator")) return "?";
            var = text(field(d, "declarator"));
            if (!literal_int(field(d, "value"), &a)) return "?";
        } else if (is(init, "assignment_expression") && text(field(init, "operator")) == "=") {
            var = text(field(init, "left"));
            if (!literal_int(field(init, "right"), &a)) return "?";
        } else {
            return "?";
        }
        if (var.empty() || text(field(cond, "left")) != var) return "?";
        if (!literal_int(field(cond, "right"), &b)) return "?";
        std::string op = text(field(cond, "operator"));

        if (is(upd, "update_expression") && text(field(upd, "argument")) == var) {
            std::string o = text(field(upd, "operator"));
            step = o == "++" ? 1 : o == "--" ? -1 : 0;
        } else if (is(upd, "assignment_expression") && text(field(upd, "left")) == var) {
            std::string o = text(field(upd, "operator"));
            long long k;
            if (!literal_int(field(upd, "right"), &k)) return "?";
            step = o == "+=" ? k : o == "-=" ? -k : 0;
        }
        if (step == 0) return "?";

        long long count;
        if (step > 0) {
            if (op == "<" || op == "!=") count = (b - a + step - 1) / step;
            else if (op == "<=") count = (b - a) / step + 1;
            else return "?";
            if (op == "!=" && (b - a) % step != 0) return "?";
        } else {
            long long s = -step;
            if (op == ">" || op == "!=") count = (a - b + s - 1) / s;
            else if (op == ">=") count = (a - b) / s + 1;
            else return "?";
            if (op == "!=" && (a - b) % s != 0) return "?";
        }
        if (count < 0) count = 0;
        return std::to_string(count);
    }

    void walk_children(TSNode n, FnCtx& fc, std::vector<Region>& out) {
        each_kid(n, [&](TSNode c) { walk(c, fc, out); });
    }

    void walk(TSNode n, FnCtx& fc, std::vector<Region>& out) {
        const char* t = ts_node_type(n);
        if (!ts_node_is_named(n)) return;
        if (!std::strcmp(t, "function_definition") || !std::strcmp(t, "class_specifier") ||
            !std::strcmp(t, "struct_specifier") || !std::strcmp(t, "comment"))
            return;

        if (!std::strcmp(t, "expression_statement")) {
            TSNode e = named0(n);
            if (!null(misparsed_fnptr(e))) {
                // a function-pointer declaration, not a call; only its initializer can call
                if (is(e, "assignment_expression")) walk(field(e, "right"), fc, out);
                return;
            }
        }
        if (!std::strcmp(t, "call_expression")) {
            TSNode fn = field(n, "function");
            if (!null(fn)) walk(fn, fc, out);  // calls nested in the callee: a().b()
            TSNode args = field(n, "arguments");
            if (!null(args)) walk(args, fc, out);  // arguments are evaluated first
            emit_call(n, fn, fc, out);
            return;
        }
        if (fc.has_error && !std::strcmp(t, "function_declarator")) {
            // Error recovery often reads a half-typed statement such as
            // `foo( bar();` as a declaration of a function `bar`. Inside a broken
            // function, a "declared" name that is a function defined in this file
            // is far more likely to be a call, so keep it in the diagram.
            walk_children(n, fc, out);
            TSNode d = field(n, "declarator");
            if (is(d, "identifier") && defined.count(text(d))) {
                Region r;
                r.kind = RegionKind::Call;
                r.name = text(d);
                r.range = range_of(n);
                out.push_back(std::move(r));
            }
            return;
        }
        bool is_for = !std::strcmp(t, "for_statement"), is_while = !std::strcmp(t, "while_statement"),
             is_do = !std::strcmp(t, "do_statement"), is_range = !std::strcmp(t, "for_range_loop");
        if (is_for || is_while || is_do || is_range) {
            Region loop;
            loop.kind = RegionKind::Loop;
            loop.range = range_of(n);
            loop.loop_kind = is_for ? "for" : is_while ? "while" : is_do ? "do" : "range-for";
            if (is_for) {
                loop.count = for_count(n);
            } else if (is_range) {
                loop.count = "?";
            } else {
                int truth = constant_truth(field(n, "condition"));
                loop.count = truth == 1 ? "inf" : (truth == 0 && is_do) ? "1" : truth == 0 ? "0" : "?";
            }
            fc.breakable.push_back('L');
            each_child(n, [&](TSNode c, std::string_view f) {
                // the initializer / range expression runs once, before the loop
                bool once = (is_for && f == "initializer") || (is_range && f == "right");
                if (once) fc.breakable.pop_back();
                walk(c, fc, once ? out : loop.children);
                if (once) fc.breakable.push_back('L');
            });
            fc.breakable.pop_back();
            out.push_back(std::move(loop));
            return;
        }
        if (!std::strcmp(t, "if_statement")) {
            walk_if(n, fc, out);
            return;
        }
        if (!std::strcmp(t, "switch_statement")) {
            walk_switch(n, fc, out);
            return;
        }
        auto simple = [&](RegionKind k, std::string name = {}) {
            Region r;
            r.kind = k;
            r.range = range_of(n);
            r.name = std::move(name);
            out.push_back(std::move(r));
        };
        if (!std::strcmp(t, "lambda_expression")) {
            // Calls in a lambda body are shown inline where the lambda is written.
            std::vector<char> saved;
            saved.swap(fc.breakable);
            ++fc.lambda_depth;
            walk_children(n, fc, out);
            --fc.lambda_depth;
            saved.swap(fc.breakable);
            return;
        }
        if (!std::strcmp(t, "return_statement") || !std::strcmp(t, "co_return_statement")) {
            walk_children(n, fc, out);
            if (fc.lambda_depth == 0) simple(RegionKind::Return);
            return;
        }
        if (!std::strcmp(t, "break_statement")) {
            if (!fc.breakable.empty() && fc.breakable.back() == 'L') simple(RegionKind::Break);
            return;
        }
        if (!std::strcmp(t, "continue_statement")) {
            for (char b : fc.breakable)
                if (b == 'L') {
                    simple(RegionKind::Continue);
                    break;
                }
            return;
        }
        if (!std::strcmp(t, "goto_statement")) {
            simple(RegionKind::Goto, text(field(n, "label")));
            return;
        }
        if (!std::strcmp(t, "labeled_statement")) {
            Region r;
            r.kind = RegionKind::Label;
            r.name = text(field(n, "label"));
            r.range = range_of(field(n, "label"));
            out.push_back(std::move(r));
            each_child(n, [&](TSNode c, std::string_view f) {
                if (f != "label") walk(c, fc, out);
            });
            return;
        }
        walk_children(n, fc, out);
    }

    void walk_if(TSNode n, FnCtx& fc, std::vector<Region>& out) {
        Region r;
        r.kind = RegionKind::If;
        r.range = range_of(n);
        TSNode cur = n;
        bool first = true;
        std::vector<Region> pre;  // calls in the first condition run before the fork
        for (int guard = 0; guard < 4096; ++guard) {
            TSNode cond = field(cur, "condition"), cons = field(cur, "consequence"),
                   alt = field(cur, "alternative");
            Region lane;
            lane.kind = RegionKind::Lane;
            lane.name = (first ? "if " : "else if ") + text(cond);
            lane.range = range_of(null(cons) ? cur : cons);
            if (!null(cond)) {
                lane.range.line = range_of(cond).line;
                lane.range.col = range_of(cond).col;
                walk(cond, fc, first ? pre : lane.children);
            }
            if (!null(cons)) walk(cons, fc, lane.children);
            r.children.push_back(std::move(lane));
            if (null(alt)) break;
            TSNode stmt = alt;
            if (is(alt, "else_clause")) {
                uint32_t k = ts_node_named_child_count(alt);
                stmt = k ? ts_node_named_child(alt, k - 1) : TSNode{};
            }
            if (is(stmt, "if_statement")) {
                cur = stmt;
                first = false;
                continue;
            }
            Region el;
            el.kind = RegionKind::Lane;
            el.name = "else";
            el.range = range_of(alt);
            if (!null(stmt)) walk(stmt, fc, el.children);
            r.children.push_back(std::move(el));
            break;
        }
        for (auto& p : pre) out.push_back(std::move(p));
        out.push_back(std::move(r));
    }

    void walk_switch(TSNode n, FnCtx& fc, std::vector<Region>& out) {
        TSNode cond = field(n, "condition"), body = field(n, "body");
        if (!null(cond)) walk(cond, fc, out);
        Region sw;
        sw.kind = RegionKind::Switch;
        sw.range = range_of(n);
        sw.name = text(cond);
        fc.breakable.push_back('S');
        std::string pending;  // labels of empty fall-through cases: "case 1, 2"
        SrcRange pending_range{};
        if (!null(body)) {
            each_child(body, [&](TSNode c, std::string_view) {
                if (!ts_node_is_named(c)) return;
                if (!is(c, "case_statement")) {
                    if (!sw.children.empty()) walk(c, fc, sw.children.back().children);
                    return;
                }
                TSNode value = field(c, "value");
                std::string label = null(value) ? "default" : text(value);
                bool empty = true;
                each_child(c, [&](TSNode s, std::string_view f) {
                    if (ts_node_is_named(s) && f != "value" && !is(s, "comment")) empty = false;
                });
                if (pending.empty()) pending_range = range_of(c);
                std::string full = pending.empty() ? (null(value) ? label : "case " + label)
                                                   : pending + ", " + label;
                if (empty) {
                    pending = full;
                    return;
                }
                Region lane;
                lane.kind = RegionKind::Lane;
                lane.name = full;
                lane.range = range_of(c);
                lane.range.line = pending_range.line;
                lane.range.col = pending_range.col;
                pending.clear();
                each_child(c, [&](TSNode s, std::string_view f) {
                    if (f != "value") walk(s, fc, lane.children);
                });
                sw.children.push_back(std::move(lane));
            });
        }
        if (!pending.empty()) {
            Region lane;
            lane.kind = RegionKind::Lane;
            lane.name = pending;
            lane.range = pending_range;
            sw.children.push_back(std::move(lane));
        }
        fc.breakable.pop_back();
        out.push_back(std::move(sw));
    }

    void emit_call(TSNode call, TSNode fn, FnCtx& fc, std::vector<Region>& out) {
        Region r;
        r.range = range_of(call);
        auto indirect = [&](const std::string& var) {
            r.kind = RegionKind::IndirectCall;
            std::string e = text(fn);
            while (e.size() > 2 && e.front() == '(' && e.back() == ')') e = e.substr(1, e.size() - 2);
            r.name = e;
            auto it = assigned.find(var);
            if (it != assigned.end()) r.candidates = it->second;
        };
        auto direct = [&](const std::string& name) {
            r.kind = RegionKind::Call;
            r.name = name;
        };
        if (null(fn)) return;
        TSNode f = fn;
        if (is(f, "template_function")) f = field(f, "name");
        if (is(f, "identifier")) {
            std::string name = text(f);
            if (fc.locals.count(name) || (!defined.count(name) && vars.count(name))) indirect(name);
            else direct(name);
        } else if (is(f, "qualified_identifier")) {
            TSNode leaf = f;
            while (is(leaf, "qualified_identifier")) leaf = field(leaf, "name");
            if (is(leaf, "template_function")) leaf = field(leaf, "name");
            std::string name = text(leaf), full;
            for (char ch : text(f))
                if (ch != ' ') full += ch;
            direct(name);
            auto cut = full.rfind("::" + name);
            // drop template arguments from the scope: "Foo<int>::bar" keeps "Foo<int>"
            if (cut != std::string::npos && cut > 0) r.qualifier = full.substr(0, cut);
        } else if (is(f, "field_expression")) {
            TSNode fld = field(f, "field");
            if (is(fld, "template_method")) fld = field(fld, "name");
            std::string name = text(fld);
            if (virtuals.count(name)) {
                indirect("");
                r.callee = name;
            } else if (defined.count(name)) {
                direct(name);
            } else if (vars.count(name)) {
                indirect(name);
            } else {
                direct(name);
            }
        } else {
            indirect(base_name(f));
        }
        if (r.kind == RegionKind::Call && r.name.empty()) return;
        out.push_back(std::move(r));
    }

    void build(TSNode root) {
        std::string top;
        scan(root, &top, false, false);
        struct Named {
            Def def;
            std::string name, qualified;
        };
        std::vector<Named> named;
        for (const auto& d : defs) {
            TSNode fd = find_function_declarator(field(d.node, "declarator"));
            if (null(fd)) continue;
            auto [name, full] = function_name(fd);
            if (name.empty()) continue;
            named.push_back({d, name, full});
            defined.insert(name);
        }
        for (const auto& nd : named) {
            Function f;
            f.name = nd.name;
            f.qualified = nd.qualified;
            f.scope = nd.def.scope;
            auto sep = f.qualified.rfind("::");
            if (sep != std::string::npos) f.scope = f.qualified.substr(0, sep);
            else if (!f.scope.empty()) f.qualified = f.scope + "::" + f.name;
            f.range = range_of(nd.def.node);
            f.has_error = ts_node_has_error(nd.def.node);
            FnCtx fc;
            fc.has_error = f.has_error;
            collect_locals(nd.def.node, fc, true);
            each_child(nd.def.node, [&](TSNode c, std::string_view fld) {
                // the body, plus constructor initializer lists / function-try-blocks
                if (fld != "declarator" && fld != "type") walk(c, fc, f.body);
            });
            model.functions.push_back(std::move(f));
        }
        resolve_calls(model);
    }
};

struct ParserHandle {
    TSParser* p = ts_parser_new();
    ~ParserHandle() { ts_parser_delete(p); }
};

class TreeSitterParser final : public Parser {
public:
    Model parse(const std::string& path, std::string_view text) override {
        bool cpp = path_is_cpp(path);
        ts_parser_set_language(handle_.p, cpp ? tree_sitter_cpp() : tree_sitter_c());
        TSTree* tree = ts_parser_parse_string(handle_.p, nullptr, text.data(), (uint32_t)text.size());
        Builder b;
        b.src = text;
        b.model.path = path;
        b.model.is_cpp = cpp;
        if (tree) {
            b.build(ts_tree_root_node(tree));
            ts_tree_delete(tree);
        }
        return std::move(b.model);
    }

private:
    ParserHandle handle_;
};

}  // namespace

std::unique_ptr<Parser> make_tree_sitter_parser() { return std::make_unique<TreeSitterParser>(); }

std::string debug_sexp(const std::string& path, std::string_view text) {
    ParserHandle h;
    ts_parser_set_language(h.p, path_is_cpp(path) ? tree_sitter_cpp() : tree_sitter_c());
    TSTree* tree = ts_parser_parse_string(h.p, nullptr, text.data(), (uint32_t)text.size());
    if (!tree) return {};
    char* s = ts_node_string(ts_tree_root_node(tree));
    std::string out = s ? s : "";
    std::free(s);
    ts_tree_delete(tree);
    return out;
}

}  // namespace tracespace
