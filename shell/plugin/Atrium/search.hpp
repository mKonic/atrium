#pragma once
// Launcher search: ranking and Spotlight-style arithmetic. Plain C++ so it
// is tested without Qt; plugin.cpp hands it to QML.

#include <optional>
#include <string>
#include <string_view>

namespace atrium::search {

// How well `query` matches `text` (both already lowercased): higher is
// better, 0 is no match. Whole name > name start > word start > initials >
// substring > letters in order.
int score(std::string_view query, std::string_view text);

// Numbers, + - * / % ^, parentheses, pi, e, sqrt/sin/cos/tan/log/ln/abs.
// Nothing when `text` is not a calculation (a lone number is not one).
std::optional<double> calculate(std::string_view text);

// A result as people read it: no float noise, no needless decimals.
std::string format_number(double value);

} // namespace atrium::search
