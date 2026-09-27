#pragma once
// Palette ranking, after Tinycast's launcher (docs/features/launcher.md there):
// an alignment scorer over one field, a sensitivity rule for what counts as a
// hit, learned frecency with the words each entry was found by, and the
// comparator that orders hits. Plain C++ so it is tested without Qt; the Qt
// side folds text (case, accents) before it gets here.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium::rank {

// A field ready to be matched: folded characters, and which of them begin a
// word in the original (after a separator, a capital after a lowercase
// letter, a letter after a digit, the last capital of a run before lowercase:
// OrbStack, 1Password, BBEdit).
struct Text {
    std::u32string folded;
    std::vector<bool> starts;
};

// ASCII lowercase; the Qt side hands in one that folds accents and case.
using Fold = std::function<std::u32string(char32_t)>;
Text prepare(std::u32string_view original, const Fold& fold = {});
std::u32string fold_all(std::u32string_view original, const Fold& fold = {});

bool is_separator(char32_t c);

// The best alignment of `query` (folded) over `text`: per matched character 4
// (the query's first on the text's first), 3 (a word start), 2 (anywhere,
// or a separator on the same one), 1 (a separator on another), less 1 when not
// right after the previous match. A query separator with nothing to land on
// is skipped. Nothing when the letters don't all appear in order.
std::optional<int> score(std::u32string_view query, const Text& text);

// How loose a hit may be (Settings, Launcher).
enum class Sensitivity { Low, Medium, High };
// L is the query's length without separators.
bool passes(int score, int length, Sensitivity sensitivity);
int query_length(std::u32string_view query);

// --- learned ranking -----------------------------------------------------------------

// Seconds; half-life 10 days: score(t) = max(1, e^(λ·(anchor − t))).
struct Visit {
    double anchor = 0;
    double last = 0;                  // when it was last opened
    std::vector<std::u32string> terms;  // the last three distinct queries it was found by, newest first
};

double frecency(const Visit& v, double now);
// Opened now, found by `term` (folded; empty: not by typing): the score rises by 100.
void record_visit(Visit& v, double now, std::u32string_view term);
// Its terms steer the order only while it is used: score above 1 and opened
// in the last 17 days.
bool terms_active(const Visit& v, double now);
// Past its anchor it scores as never opened: nothing worth keeping.
bool expired(const Visit& v, double now);

// --- ordering ------------------------------------------------------------------------

// What one entry is, for one query.
struct Facts {
    bool alias_exact = false;
    bool alias_prefix = false;
    bool exact_title = false;     // title or an alternate title, queries past three characters
    int term = 0;                 // a learned search term: 3 equal, 2 starting with the query, 1 the query runs ≤3 past it
    bool exact_subtitle = false;
    int best = 0;                 // best score over title, alternate titles, subtitle
    double frecency = 1;
    int title_score = 0;
    bool title_prefix = false;    // title or alternate title starts with the query
    int kind_priority = 0;        // applications 4, commands 3, quicklinks 2, settings 1
    bool has_alias = false;
    std::u32string name;          // folded title, compared numerically
};

// Whether `a` goes before `b` for a typed query.
bool before(const Facts& a, const Facts& b);
// With nothing typed: frecency, having an alias, kind priority, the name.
bool before_idle(const Facts& a, const Facts& b);
// Names with their digits compared as numbers ("Space 2" before "Space 10").
int compare_names(std::u32string_view a, std::u32string_view b);

// Everything a candidate offers to be matched on.
struct Fields {
    Text title;
    std::vector<Text> alternates;
    Text subtitle;
    std::vector<Text> keywords;  // make an entry appear, never rank it
    std::u32string alias;        // folded, "" none
};

// Whether the entry shows for `query` and, if so, its facts. `visit` may be null.
std::optional<Facts> match(std::u32string_view query, const Fields& fields, const Visit* visit, double now,
                           Sensitivity sensitivity, int kind_priority);
// The facts with nothing typed.
Facts idle(const Fields& fields, const Visit* visit, double now, int kind_priority);

} // namespace atrium::rank
