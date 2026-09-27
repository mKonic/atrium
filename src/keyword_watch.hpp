#pragma once
// Snippet keywords typed in any app (launcher.snippet_expansion): the last
// characters typed into the focused window, checked against the keywords
// after each one. Typing is all it sees: a keystroke that moves the caret
// (an arrow, Enter, a click, a chord) or a focus change starts it over, so a
// keyword only counts when it was typed in one go. Plain C++, tested alone.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

class KeywordWatch {
public:
    // (snippet id, keyword); empty keywords are ignored.
    void set_keywords(std::vector<std::pair<int64_t, std::string>> keywords);
    bool empty() const { return keywords_.empty(); }

    // A character typed. The snippet whose keyword it completed, if any (the
    // longest, when one keyword ends another); the typing starts over then.
    std::optional<std::pair<int64_t, std::string>> typed(char32_t c);
    void backspace();
    void reset() { typed_.clear(); }

private:
    std::vector<std::pair<int64_t, std::u32string>> keywords_;
    std::vector<std::string> utf8_;
    size_t longest_ = 0;
    std::u32string typed_;
};

} // namespace atrium
