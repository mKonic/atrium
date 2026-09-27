#include "keyword_watch.hpp"

#include <algorithm>

namespace atrium {

namespace {

std::u32string decode(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 1;
        char32_t cp = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
        for (int k = 1; k < n && i + size_t(k) < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3f);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}

} // namespace

void KeywordWatch::set_keywords(std::vector<std::pair<int64_t, std::string>> keywords) {
    keywords_.clear();
    utf8_.clear();
    longest_ = 0;
    // Longest first: ";sig2" wins over ";sig" when both end the typing.
    std::ranges::stable_sort(keywords, [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
    for (auto& [id, word] : keywords) {
        if (word.empty())
            continue;
        std::u32string w = decode(word);
        longest_ = std::max(longest_, w.size());
        keywords_.emplace_back(id, std::move(w));
        utf8_.push_back(word);
    }
    typed_.clear();
}

std::optional<std::pair<int64_t, std::string>> KeywordWatch::typed(char32_t c) {
    if (keywords_.empty())
        return std::nullopt;
    typed_.push_back(c);
    // Only as much as the longest keyword can use.
    if (typed_.size() > longest_)
        typed_.erase(0, typed_.size() - longest_);
    for (size_t i = 0; i < keywords_.size(); ++i)
        if (typed_.ends_with(keywords_[i].second)) {
            typed_.clear();
            return std::pair{keywords_[i].first, utf8_[i]};
        }
    return std::nullopt;
}

void KeywordWatch::backspace() {
    if (!typed_.empty())
        typed_.pop_back();
}

} // namespace atrium
