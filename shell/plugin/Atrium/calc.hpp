#pragma once
// The palette's calculator, after Tinycast's (docs/features/calculator.md
// there), as far as a Linux desktop needs it: arithmetic with units that
// carry through (10kg + 500g, 100km / 2h to km/h), conversions (10 km to mi,
// 72f in c, 1 GiB to MB), a bare quantity shown in the other system (5 ft →
// 1.524 m), currencies from a rates table the caller fetches ($20 in eur),
// percentages (15% of 80, 80 + 15%, 15% off 80), number bases (255 to hex,
// 0xff), the time in a city (time in tokyo, 5pm london in tokyo) and dates
// (days until 25 dec, today + 3 weeks, 2 weeks ago).
//
// Pure: the clock, the time zone, the rates and the home currency are passed
// in, so it is tested without a network or a date. Numbers in and out are
// written the English way (1,234.5); the Qt side localizes.

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace atrium::calc {

struct Rates {
    // Units of each currency (ISO code) per euro, as the ECB publishes them.
    std::map<std::string, double> per_euro;
    std::string date;  // the day they are from
};

struct Context {
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::string zone;             // IANA name of the local zone; "" the system's
    const Rates* rates = nullptr; // none yet: currencies stay silent
    std::string home_currency = "USD";
};

struct Answer {
    std::string input;        // the question as the card shows it
    std::string input_badge;  // what it was ("Metres", "USD", "Tokyo")
    std::string result;       // the answer as the card shows it ("1.524 m")
    std::string result_badge; // what it is ("Feet", "Friday")
    std::string copy;         // what Enter copies: the answer, without grouping
};

// Nothing when `query` isn't a calculation (a lone number or word isn't one).
std::optional<Answer> evaluate(std::string_view query, const Context& context);

// Parse the ECB's eurofxref-daily.xml.
std::optional<Rates> parse_ecb(std::string_view xml);

// A number as people read it: grouped, no float noise, at most `digits`
// significant digits.
std::string format(double value, int digits = 10);

} // namespace atrium::calc
