#pragma once
// Search Files' rules, free of Qt so they are tested on their own: which
// paths a search may show (inside a chosen folder, nothing hidden on the way,
// nothing the ignore patterns name), what the typed words ask for, and the
// recently used files desktop apps record (recently-used.xbel).

#include <string>
#include <string_view>
#include <vector>

namespace atrium::file_search {

// Ignore patterns, compiled once: a bare name ("node_modules") matches any
// path component; one with * ? [ is a glob over any component ("*.tmp"); one
// with a / is a glob over the whole path ("**/cache/**"). Case-insensitive.
class IgnoreList {
public:
    explicit IgnoreList(const std::vector<std::string>& patterns);
    bool ignores(std::string_view path) const;
    // The shipped ones, always on.
    static const std::vector<std::string>& defaults();

private:
    std::vector<std::string> names_, component_globs_, path_globs_;
};

// Whether `path` may be shown: under one of `roots` (absolute, no trailing
// slash), no component below the root hidden (a dot name), not ignored.
bool admitted(std::string_view path, const std::vector<std::string>& roots, const IgnoreList& ignore);

// The query's words, lowercased: all must be in a file's name, in any order.
std::vector<std::string> terms(std::string_view query);
bool name_matches(std::string_view name, const std::vector<std::string>& terms);

// What a type filter admits, by the file's MIME type ("inode/directory" for
// folders): all, folders, documents, images, audio, videos, archives.
bool filter_accepts(std::string_view filter, std::string_view mime);

struct Recent {
    std::string path;
    long long when = 0;  // unix seconds, the latest of visited and modified
};
// Local files in a recently-used.xbel, newest first (not checked on disk).
std::vector<Recent> parse_recent(std::string_view xbel);

} // namespace atrium::file_search
