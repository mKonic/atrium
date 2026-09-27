#pragma once
// Notes' naming rules, free of Qt so they are tested alone: which names are
// placeholders, what an unnamed note shows as its title, and the free name a
// new or renamed note takes.

#include <functional>
#include <string>
#include <vector>

namespace atrium::notes {

// "Untitled", "Untitled 2", ...: names the app gave, not the user.
bool unnamed(const std::string& name);
// The first line with visible text, Markdown markers dropped, at most 120
// characters; "" when there is none.
std::string first_line(const std::string& text);
// `wanted`, or "wanted 2", "wanted 3", ... when another file (`taken`, with
// .md) has that name, compared by `fold` (case and accents). `self` never
// clashes with itself.
std::string free_name(const std::string& wanted, const std::vector<std::string>& taken, const std::string& self,
                      const std::function<std::string(const std::string&)>& fold);

// Enter at `cursor` (a byte offset) in a list item: what to do.
struct Newline {
    enum { Plain, Continue, EndList } kind = Plain;
    std::string insert;       // Continue: "\n- " (the next item's marker)
    size_t line_start = 0;    // EndList: the empty item [line_start, cursor) goes
};
Newline newline(const std::string& text, size_t cursor);

} // namespace atrium::notes
