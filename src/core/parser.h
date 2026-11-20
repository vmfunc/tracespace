// Parser layer: source text -> Model. The tree-sitter implementation resolves
// calls by name only; a libclang "precise mode" can implement the same interface.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "core/model.h"

namespace tracespace {

class Parser {
public:
    virtual ~Parser() = default;
    // Never fails: broken code yields a partial model with errors recorded.
    virtual Model parse(const std::string& path, std::string_view text) = 0;
};

std::unique_ptr<Parser> make_tree_sitter_parser();

// .c / .h are parsed as C, everything else as C++.
bool path_is_cpp(const std::string& path);

// Debug aid: the raw tree-sitter S-expression.
std::string debug_sexp(const std::string& path, std::string_view text);

}  // namespace tracespace
