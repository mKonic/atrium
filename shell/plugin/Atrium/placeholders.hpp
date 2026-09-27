#pragma once
// {placeholders} in quicklinks, snippets and custom commands, one syntax for
// all three (Tinycast's, trimmed to what Linux has):
//
//   {argument}                 asked for when run; the query when a fallback
//   {argument name="Query"}    the same, named; the same name twice is one value
//   {argument name="x" default="y"}  optional, y when left empty
//   {clipboard}                the clipboard's text
//   {date} {time} {datetime}   now, in the locale's short form
//   {uuid}                     a fresh random UUID
//   {cursor}                   where the cursor ends up (snippets); removed
//
// Plain C++ so it is tested without Qt.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace atrium::placeholders {

struct Argument {
    std::string name;  // "Query" for a bare {argument}
    std::optional<std::string> fallback;  // default="..."
};

// The arguments `text` asks for, in the order they first appear.
std::vector<Argument> arguments(const std::string& text);

// How values go in: as they are, URL-encoded (quicklinks to web addresses),
// or quoted for sh (custom commands).
enum class Encoding { Plain, Url, Shell };

struct Values {
    std::vector<std::string> arguments;  // in arguments() order; missing ones are empty
    std::string clipboard;
    std::string date, time, datetime;
    std::function<std::string()> uuid;
};

// `text` with every placeholder replaced. Unknown {words} stay as they are.
std::string expand(const std::string& text, const Values& values, Encoding encoding);

// Where {cursor} was, as a count of characters from the end of expand()'s
// result (so a caller can step back that far); nothing without one.
std::optional<size_t> cursor_from_end(const std::string& text, const Values& values);

std::string url_encode(const std::string& s);
std::string shell_quote(const std::string& s);

} // namespace atrium::placeholders
