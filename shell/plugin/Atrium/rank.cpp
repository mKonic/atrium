#include "rank.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atrium::rank {

namespace {

constexpr int kNone = std::numeric_limits<int>::min() / 4;
constexpr double kDay = 86400;
const double kLambda = std::log(2.0) / (10 * kDay);
constexpr double kTermDays = 17;

bool lower(char32_t c) {
    return (c >= U'a' && c <= U'z');
}
bool upper(char32_t c) {
    return (c >= U'A' && c <= U'Z');
}
bool digit(char32_t c) {
    return c >= U'0' && c <= U'9';
}
bool letter(char32_t c) {
    return lower(c) || upper(c) || c > 0x7f;
}

std::u32string fold_one(char32_t c, const Fold& fold) {
    if (fold)
        return fold(c);
    if (upper(c))
        return std::u32string(1, c - U'A' + U'a');
    return std::u32string(1, c);
}

} // namespace

bool is_separator(char32_t c) {
    switch (c) {
    case U' ': case U'\t': case U'\n': case U'-': case U'_': case U'.': case U'/':
    case U'(': case U')': case U'[': case U']':
        return true;
    default:
        return false;
    }
}

Text prepare(std::u32string_view s, const Fold& fold) {
    Text t;
    for (size_t i = 0; i < s.size(); ++i) {
        const char32_t c = s[i];
        bool start = false;
        if (!is_separator(c)) {
            if (i == 0 || is_separator(s[i - 1]))
                start = true;
            else if (upper(c) && lower(s[i - 1]))
                start = true;  // OrbStack
            else if (letter(c) && digit(s[i - 1]))
                start = true;  // 1Password
            else if (upper(c) && upper(s[i - 1]) && i + 1 < s.size() && lower(s[i + 1]))
                start = true;  // BBEdit
        }
        const std::u32string f = fold_one(c, fold);
        for (size_t k = 0; k < f.size(); ++k) {
            t.folded.push_back(f[k]);
            t.starts.push_back(start && k == 0);
        }
    }
    return t;
}

std::u32string fold_all(std::u32string_view s, const Fold& fold) {
    std::u32string out;
    for (char32_t c : s)
        out += fold_one(c, fold);
    return out;
}

std::optional<int> score(std::u32string_view q, const Text& text) {
    const std::u32string& t = text.folded;
    const size_t n = t.size();
    if (q.empty() || n == 0)
        return std::nullopt;
    std::vector<int> prev(n, kNone), cur(n);
    bool empty_ok = true;  // nothing matched yet is still a valid state
    bool first = true;     // the query's first character (separators skipped count)
    for (const char32_t qc : q) {
        const bool sep = is_separator(qc);
        std::fill(cur.begin(), cur.end(), kNone);
        int best_before = kNone;  // max prev[0 .. j-2]
        for (size_t j = 0; j < n; ++j) {
            if (j >= 2)
                best_before = std::max(best_before, prev[j - 2]);
            int pts;
            if (sep) {
                if (!is_separator(t[j]))
                    continue;
                pts = t[j] == qc ? 2 : 1;
            } else {
                if (t[j] != qc)
                    continue;
                pts = first && j == 0 ? 4 : text.starts[j] ? 3 : 2;
            }
            int from = kNone;
            if (j >= 1 && prev[j - 1] > kNone)
                from = prev[j - 1];
            if (best_before > kNone)
                from = std::max(from, best_before - 1);
            if (empty_ok)
                from = std::max(from, 0);
            if (from > kNone)
                cur[j] = from + pts;
        }
        if (sep) {
            // Skipped: the previous state carries over unchanged.
            for (size_t j = 0; j < n; ++j)
                cur[j] = std::max(cur[j], prev[j]);
        } else {
            empty_ok = false;
            first = false;
            if (std::ranges::all_of(cur, [](int v) { return v == kNone; }))
                return std::nullopt;
        }
        std::swap(prev, cur);
    }
    const int best = *std::ranges::max_element(prev);
    if (best == kNone)
        return std::nullopt;
    return best;
}

int query_length(std::u32string_view q) {
    return int(std::ranges::count_if(q, [](char32_t c) { return !is_separator(c); }));
}

bool passes(int score, int length, Sensitivity s) {
    switch (s) {
    case Sensitivity::Low: return true;
    case Sensitivity::Medium: return score >= 1.5 * (length - 2) + 4;
    case Sensitivity::High: return score > 2 * length;
    }
    return true;
}

// --- learned ranking -----------------------------------------------------------------

double frecency(const Visit& v, double now) {
    return std::max(1.0, std::exp(kLambda * (v.anchor - now)));
}

void record_visit(Visit& v, double now, std::u32string_view term) {
    v.anchor = now + std::log(frecency(v, now) + 100) / kLambda;
    v.last = now;
    if (!term.empty()) {
        std::erase(v.terms, std::u32string(term));
        v.terms.insert(v.terms.begin(), std::u32string(term));
        if (v.terms.size() > 3)
            v.terms.resize(3);
    }
}

bool terms_active(const Visit& v, double now) {
    return frecency(v, now) > 1 && now - v.last < kTermDays * kDay;
}

bool expired(const Visit& v, double now) {
    return v.anchor <= now;
}

// --- ordering ------------------------------------------------------------------------

int compare_names(std::u32string_view a, std::u32string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && digit(a[ie]))
                ++ie;
            while (je < b.size() && digit(b[je]))
                ++je;
            // Compare by value: strip zeros, then length, then digits.
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == U'0')
                ++is;
            while (js + 1 < je && b[js] == U'0')
                ++js;
            if (ie - is != je - js)
                return ie - is < je - js ? -1 : 1;
            for (; is < ie; ++is, ++js)
                if (a[is] != b[js])
                    return a[is] < b[js] ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        if (a[i] != b[j])
            return a[i] < b[j] ? -1 : 1;
        ++i;
        ++j;
    }
    if (i == a.size() && j == b.size())
        return 0;
    return i == a.size() ? -1 : 1;
}

namespace {

// -1: a first, 1: b first, 0: undecided.
int flag(bool a, bool b) {
    return a == b ? 0 : a ? -1 : 1;
}
template <class T>
int more(T a, T b) {
    return a == b ? 0 : a > b ? -1 : 1;
}

int tiebreak(const Facts& a, const Facts& b) {
    if (int r = more(a.frecency, b.frecency))
        return r;
    if (int r = flag(a.has_alias, b.has_alias))
        return r;
    if (int r = more(a.kind_priority, b.kind_priority))
        return r;
    return compare_names(a.name, b.name);
}

int compare(const Facts& a, const Facts& b) {
    if (int r = flag(a.alias_exact, b.alias_exact))
        return r;
    if (a.exact_title && b.exact_title) {
        if (int r = more(a.term, b.term))
            return r;
        return tiebreak(a, b);
    }
    if (int r = flag(a.exact_title, b.exact_title))
        return r;
    if (a.term == 3 && b.term == 3)
        return tiebreak(a, b);
    if (int r = flag(a.term == 3, b.term == 3))
        return r;
    if (a.exact_subtitle && b.exact_subtitle) {
        if (int r = more(a.frecency, b.frecency))
            return r;
        if (int r = more(a.title_score, b.title_score))
            return r;
        return tiebreak(a, b);
    }
    if (int r = flag(a.exact_subtitle, b.exact_subtitle))
        return r;
    if (int r = flag(a.alias_prefix, b.alias_prefix))
        return r;
    if (int r = flag(a.term == 2, b.term == 2))
        return r;
    if (int r = flag(a.term == 1, b.term == 1))
        return r;
    if (int r = more(a.best, b.best))
        return r;
    if (int r = more(a.frecency, b.frecency))
        return r;
    if (int r = more(a.title_score, b.title_score))
        return r;
    if (int r = flag(a.title_prefix, b.title_prefix))
        return r;
    if (int r = more(a.kind_priority, b.kind_priority))
        return r;
    return compare_names(a.name, b.name);
}

} // namespace

bool before(const Facts& a, const Facts& b) {
    return compare(a, b) < 0;
}

bool before_idle(const Facts& a, const Facts& b) {
    return tiebreak(a, b) < 0;
}

Facts idle(const Fields& f, const Visit* visit, double now, int kind_priority) {
    Facts facts;
    facts.frecency = visit ? frecency(*visit, now) : 1;
    facts.kind_priority = kind_priority;
    facts.has_alias = !f.alias.empty();
    facts.name = f.title.folded;
    return facts;
}

std::optional<Facts> match(std::u32string_view q, const Fields& f, const Visit* visit, double now, Sensitivity s,
                           int kind_priority) {
    Facts facts = idle(f, visit, now, kind_priority);
    if (q.empty())
        return facts;
    const int length = query_length(q);
    bool shows = false;
    auto hit = [&](const Text& t) -> std::optional<int> {
        auto sc = score(q, t);
        if (sc && passes(*sc, length, s))
            return sc;
        return std::nullopt;
    };
    auto exact = [&](const Text& t) { return t.folded == q; };
    auto prefix = [&](const Text& t) { return t.folded.starts_with(q); };

    if (!f.alias.empty()) {
        facts.alias_exact = f.alias == q;
        facts.alias_prefix = !facts.alias_exact && f.alias.starts_with(q);
        shows = facts.alias_exact || facts.alias_prefix;
    }
    const bool long_query = q.size() > 3;
    if (auto sc = hit(f.title)) {
        shows = true;
        facts.title_score = *sc;
        facts.best = std::max(facts.best, *sc);
    }
    facts.exact_title = long_query && exact(f.title);
    facts.title_prefix = prefix(f.title);
    auto alternate = [&](const Text& t) {
        if (auto sc = hit(t)) {
            shows = true;
            facts.best = std::max(facts.best, *sc);
        }
        facts.exact_title = facts.exact_title || (long_query && exact(t));
        facts.title_prefix = facts.title_prefix || prefix(t);
    };
    for (const Text& t : f.alternates)
        alternate(t);
    // An alias hit anywhere but its start counts as one more alternate title.
    if (!f.alias.empty() && !facts.alias_exact && !facts.alias_prefix)
        alternate(prepare(f.alias));
    if (auto sc = hit(f.subtitle)) {
        shows = true;
        facts.best = std::max(facts.best, *sc);
    }
    facts.exact_subtitle = !f.subtitle.folded.empty() && exact(f.subtitle);
    for (const Text& t : f.keywords)
        if (!shows && hit(t))
            shows = true;
    if (visit && terms_active(*visit, now)) {
        for (const std::u32string& term : visit->terms) {
            int strength = 0;
            if (term == q)
                strength = 3;
            else if (term.starts_with(q))
                strength = 2;
            else if (term.size() >= 3 && q.starts_with(term) && q.size() - term.size() <= 3)
                strength = 1;
            facts.term = std::max(facts.term, strength);
        }
        shows = shows || facts.term > 0;
    }
    if (!shows)
        return std::nullopt;
    return facts;
}

} // namespace atrium::rank
