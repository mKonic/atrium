#include "calc.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

namespace atrium::calc {

namespace {

using namespace std::chrono;

// --- numbers ---------------------------------------------------------------------------

std::string group(std::string digits) {
    // "1234567" → "1,234,567"
    std::string out;
    int n = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (n && n % 3 == 0)
            out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        ++n;
    }
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string(s);
}

std::string format_fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, std::fabs(v));
    std::string s = buf;
    const size_t dot = s.find('.');
    std::string whole = dot == std::string::npos ? s : s.substr(0, dot);
    std::string frac = dot == std::string::npos ? "" : s.substr(dot);
    return (v < 0 && std::fabs(v) >= std::pow(10.0, -decimals) / 2 ? "-" : "") + group(whole) + frac;
}

} // namespace

std::string format(double value, int digits) {
    if (!std::isfinite(value))
        return value > 0 ? "∞" : value < 0 ? "-∞" : "NaN";
    if (value == 0)
        return "0";
    const double mag = std::fabs(value);
    if (mag >= 1e15 || mag < 1e-9) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*g", digits, value);
        return buf;
    }
    // Significant digits, then trailing zeros dropped: 0.1+0.2 reads 0.3.
    const int int_digits = mag >= 1 ? int(std::floor(std::log10(mag))) + 1 : 0;
    int decimals = std::clamp(digits - int_digits, 0, 12);
    if (mag < 1)
        decimals = std::clamp(digits - int(std::floor(std::log10(mag))) - 1, 0, 15);
    std::string s = format_fixed(value, decimals);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

namespace {

// --- units ------------------------------------------------------------------------------

// Exponents of length, mass, time, data (bytes), money (euros), angle.
constexpr int kDims = 6;
using Dim = std::array<int, kDims>;
enum { L, M, T, D, C, A };

Dim dim(int l = 0, int m = 0, int t = 0, int d = 0, int c = 0, int a = 0) {
    return {l, m, t, d, c, a};
}
Dim add(Dim a, const Dim& b, int sign = 1) {
    for (int i = 0; i < kDims; ++i)
        a[size_t(i)] += sign * b[size_t(i)];
    return a;
}
Dim scale(Dim a, int k) {
    for (int& x : a)
        x *= k;
    return a;
}
bool none(const Dim& d) {
    return std::ranges::all_of(d, [](int x) { return x == 0; });
}

struct Unit {
    std::string symbol;  // how answers write it
    std::string name;    // the badge ("Metres")
    Dim dim;
    double factor = 1;       // in base units (m, kg, s, byte, euro, rad)
    double offset = 0;       // temperatures: K = (v + offset) * factor
    bool temperature = false;
    std::string currency;    // ISO code for money
    std::string partner;     // what a bare quantity is also shown in
};

struct Catalog {
    std::vector<Unit> units;
    std::map<std::string, size_t> exact;    // case-sensitive spellings (MB vs Mb)
    std::map<std::string, size_t> folded;   // the rest, lowercase

    size_t add(Unit u, std::initializer_list<const char*> exact_names, std::initializer_list<const char*> names) {
        units.push_back(std::move(u));
        const size_t i = units.size() - 1;
        for (const char* n : exact_names)
            exact.emplace(n, i);
        for (const char* n : names)
            folded.emplace(lower(n), i);
        return i;
    }

    const Unit* find(const std::string& word) const {
        if (auto it = exact.find(word); it != exact.end())
            return &units[it->second];
        if (auto it = folded.find(lower(word)); it != folded.end())
            return &units[it->second];
        return nullptr;
    }
};

const Catalog& catalog() {
    static const Catalog c = [] {
        Catalog k;
        auto u = [&](std::string sym, std::string name, Dim d, double f, std::string partner,
                     std::initializer_list<const char*> exact, std::initializer_list<const char*> names) {
            k.add({sym, name, d, f, 0, false, "", partner}, exact, names);
        };
        const Dim len = dim(1), mass = dim(0, 1), time = dim(0, 0, 1), data = dim(0, 0, 0, 1);
        // Length
        u("mm", "Millimetres", len, 1e-3, "in", {}, {"mm", "millimeter", "millimeters", "millimetre", "millimetres"});
        u("cm", "Centimetres", len, 1e-2, "in", {}, {"cm", "centimeter", "centimeters", "centimetre", "centimetres"});
        u("m", "Metres", len, 1, "ft", {}, {"m", "meter", "meters", "metre", "metres"});
        u("km", "Kilometres", len, 1e3, "mi", {}, {"km", "kilometer", "kilometers", "kilometre", "kilometres"});
        u("µm", "Micrometres", len, 1e-6, "", {}, {"um", "µm", "micrometer", "micrometers", "micron", "microns"});
        u("nm", "Nanometres", len, 1e-9, "", {}, {"nm", "nanometer", "nanometers"});
        u("in", "Inches", len, 0.0254, "cm", {"\""}, {"in", "inch", "inches"});
        u("ft", "Feet", len, 0.3048, "m", {"'"}, {"ft", "foot", "feet"});
        u("yd", "Yards", len, 0.9144, "m", {}, {"yd", "yard", "yards"});
        u("mi", "Miles", len, 1609.344, "km", {}, {"mi", "mile", "miles"});
        u("nmi", "Nautical Miles", len, 1852, "km", {}, {"nmi", "nauticalmile", "nauticalmiles"});
        // Mass
        u("mg", "Milligrams", mass, 1e-6, "", {}, {"mg", "milligram", "milligrams"});
        u("g", "Grams", mass, 1e-3, "oz", {}, {"g", "gram", "grams", "gramme", "grammes"});
        u("kg", "Kilograms", mass, 1, "lb", {}, {"kg", "kilo", "kilos", "kilogram", "kilograms"});
        u("t", "Tonnes", mass, 1e3, "lb", {}, {"t", "tonne", "tonnes", "ton", "tons"});
        u("oz", "Ounces", mass, 0.028349523125, "g", {}, {"oz", "ounce", "ounces"});
        u("lb", "Pounds", mass, 0.45359237, "kg", {}, {"lb", "lbs", "pound", "pounds"});
        u("st", "Stone", mass, 6.35029318, "kg", {}, {"st", "stone", "stones"});
        // Time
        u("ms", "Milliseconds", time, 1e-3, "", {}, {"ms", "millisecond", "milliseconds"});
        u("s", "Seconds", time, 1, "", {}, {"s", "sec", "secs", "second", "seconds"});
        u("min", "Minutes", time, 60, "s", {}, {"min", "mins", "minute", "minutes"});
        u("hr", "Hours", time, 3600, "min", {}, {"h", "hr", "hrs", "hour", "hours"});
        u("day", "Days", time, 86400, "hr", {}, {"d", "day", "days"});
        u("wk", "Weeks", time, 604800, "day", {}, {"w", "wk", "wks", "week", "weeks"});
        u("mo", "Months", time, 2629746, "day", {}, {"mo", "month", "months"});
        u("yr", "Years", time, 31556952, "day", {}, {"y", "yr", "yrs", "year", "years"});
        // Data: B bytes, b bits; decimal and binary prefixes.
        u("B", "Bytes", data, 1, "", {"B"}, {"byte", "bytes"});
        u("bit", "Bits", data, 0.125, "B", {"b"}, {"bit", "bits"});
        const std::pair<const char*, double> dec[] = {{"k", 1e3}, {"M", 1e6}, {"G", 1e9}, {"T", 1e12}, {"P", 1e15}};
        const std::pair<const char*, double> bin[] = {{"Ki", 1024.0}, {"Mi", 1048576.0}, {"Gi", 1073741824.0},
                                                      {"Ti", 1099511627776.0}, {"Pi", 1125899906842624.0}};
        const char* names[] = {"Kilobytes", "Megabytes", "Gigabytes", "Terabytes", "Petabytes"};
        const char* bnames[] = {"Kibibytes", "Mebibytes", "Gibibytes", "Tebibytes", "Pebibytes"};
        for (int i = 0; i < 5; ++i) {
            const std::string p = dec[i].first, P = i == 0 ? "K" : p;
            const std::string B = P + "B", bi = std::string(bin[i].first) + "B";
            const std::string bits = P + "b", bitsl = p + "bit";
            k.add({i == 0 ? "kB" : B, names[i], data, dec[i].second, 0, false, "", bi}, {B.c_str(), (p + "B").c_str()},
                  {B.c_str()});  // mb, gb read as bytes, the everyday meaning
            k.add({bi, bnames[i], data, bin[i].second, 0, false, "", i == 0 ? "kB" : B}, {bi.c_str()}, {bi.c_str()});
            k.add({P + "bit", std::string(names[i]).replace(std::string(names[i]).find("bytes"), 5, "bits"), data,
                   dec[i].second / 8, 0, false, "", i == 0 ? "kB" : B},
                  {bits.c_str()}, {bitsl.c_str()});
        }
        // Area and volume
        const Dim area = dim(2), vol = dim(3);
        u("m²", "Square Metres", area, 1, "ft²", {}, {"m2", "m²", "sqm"});
        u("cm²", "Square Centimetres", area, 1e-4, "in²", {}, {"cm2", "cm²"});
        u("mm²", "Square Millimetres", area, 1e-6, "", {}, {"mm2", "mm²"});
        u("km²", "Square Kilometres", area, 1e6, "mi²", {}, {"km2", "km²"});
        u("in²", "Square Inches", area, 0.00064516, "cm²", {}, {"in2", "in²", "sqin"});
        u("ft²", "Square Feet", area, 0.09290304, "m²", {}, {"ft2", "ft²", "sqft"});
        u("mi²", "Square Miles", area, 2589988.110336, "km²", {}, {"mi2", "mi²"});
        u("ha", "Hectares", area, 1e4, "ac", {}, {"ha", "hectare", "hectares"});
        u("ac", "Acres", area, 4046.8564224, "ha", {}, {"ac", "acre", "acres"});
        u("mL", "Millilitres", vol, 1e-6, "fl oz", {}, {"ml", "milliliter", "milliliters", "millilitre", "millilitres"});
        u("cL", "Centilitres", vol, 1e-5, "", {}, {"cl"});
        u("dL", "Decilitres", vol, 1e-4, "", {}, {"dl"});
        u("L", "Litres", vol, 1e-3, "gal", {}, {"l", "liter", "liters", "litre", "litres"});
        u("m³", "Cubic Metres", vol, 1, "L", {}, {"m3", "m³"});
        u("cm³", "Cubic Centimetres", vol, 1e-6, "mL", {}, {"cm3", "cm³", "cc"});
        u("ft³", "Cubic Feet", vol, 0.028316846592, "L", {}, {"ft3", "ft³"});
        u("gal", "Gallons", vol, 3.785411784e-3, "L", {}, {"gal", "gallon", "gallons"});
        u("qt", "Quarts", vol, 9.46352946e-4, "L", {}, {"qt", "quart", "quarts"});
        u("pt", "Pints", vol, 4.73176473e-4, "mL", {}, {"pt", "pint", "pints"});
        u("cup", "Cups", vol, 2.365882365e-4, "mL", {}, {"cup", "cups"});
        u("fl oz", "Fluid Ounces", vol, 2.95735295625e-5, "mL", {}, {"floz", "fl oz"});
        u("tbsp", "Tablespoons", vol, 1.478676478125e-5, "mL", {}, {"tbsp", "tablespoon", "tablespoons"});
        u("tsp", "Teaspoons", vol, 4.92892159375e-6, "mL", {}, {"tsp", "teaspoon", "teaspoons"});
        // Speed and frequency
        const Dim speed = dim(1, 0, -1);
        u("m/s", "Metres per Second", speed, 1, "km/h", {}, {"m/s", "mps"});
        u("km/h", "Kilometres per Hour", speed, 1 / 3.6, "mph", {}, {"km/h", "kmh", "kph", "kmph"});
        u("mph", "Miles per Hour", speed, 0.44704, "km/h", {}, {"mph", "mi/h"});
        u("kn", "Knots", speed, 1852.0 / 3600, "km/h", {}, {"kn", "kt", "knot", "knots"});
        u("ft/s", "Feet per Second", speed, 0.3048, "m/s", {}, {"ft/s", "fps"});
        u("Hz", "Hertz", dim(0, 0, -1), 1, "", {}, {"hz", "hertz"});
        u("kHz", "Kilohertz", dim(0, 0, -1), 1e3, "", {}, {"khz"});
        u("MHz", "Megahertz", dim(0, 0, -1), 1e6, "", {}, {"mhz"});
        u("GHz", "Gigahertz", dim(0, 0, -1), 1e9, "", {}, {"ghz"});
        // Data rates
        const Dim rate = dim(0, 0, -1, 1);
        u("B/s", "Bytes per Second", rate, 1, "", {}, {"b/s"});
        u("kbps", "Kilobits per Second", rate, 125, "", {}, {"kbps", "kbit/s"});
        u("Mbps", "Megabits per Second", rate, 125000, "MB/s", {}, {"mbps", "mbit/s"});
        u("Gbps", "Gigabits per Second", rate, 1.25e8, "MB/s", {}, {"gbps", "gbit/s"});
        u("MB/s", "Megabytes per Second", rate, 1e6, "Mbps", {}, {"mb/s"});
        u("GB/s", "Gigabytes per Second", rate, 1e9, "Gbps", {}, {"gb/s"});
        // Energy, power, pressure, force
        const Dim energy = dim(2, 1, -2), power = dim(2, 1, -3), pressure = dim(-1, 1, -2);
        u("J", "Joules", energy, 1, "cal", {}, {"j", "joule", "joules"});
        u("kJ", "Kilojoules", energy, 1e3, "kcal", {}, {"kj", "kilojoule", "kilojoules"});
        u("cal", "Calories", energy, 4.184, "J", {}, {"cal", "calorie", "calories"});
        u("kcal", "Kilocalories", energy, 4184, "kJ", {}, {"kcal", "kilocalorie", "kilocalories"});
        u("Wh", "Watt-hours", energy, 3600, "kJ", {}, {"wh"});
        u("kWh", "Kilowatt-hours", energy, 3.6e6, "MJ", {}, {"kwh"});
        u("MJ", "Megajoules", energy, 1e6, "kWh", {}, {"mj"});
        u("W", "Watts", power, 1, "", {"W"}, {"watt", "watts"});
        u("kW", "Kilowatts", power, 1e3, "hp", {}, {"kw", "kilowatt", "kilowatts"});
        u("MW", "Megawatts", power, 1e6, "", {"MW"}, {"megawatt", "megawatts"});
        u("hp", "Horsepower", power, 745.69987158227022, "kW", {}, {"hp", "horsepower"});
        u("Pa", "Pascals", pressure, 1, "", {}, {"pa", "pascal", "pascals"});
        u("hPa", "Hectopascals", pressure, 100, "", {}, {"hpa"});
        u("kPa", "Kilopascals", pressure, 1e3, "psi", {}, {"kpa"});
        u("bar", "Bar", pressure, 1e5, "psi", {}, {"bar", "bars"});
        u("psi", "Pounds per Square Inch", pressure, 6894.757293168, "bar", {}, {"psi"});
        u("atm", "Atmospheres", pressure, 101325, "bar", {}, {"atm"});
        u("mmHg", "Millimetres of Mercury", pressure, 133.322387415, "kPa", {}, {"mmhg"});
        u("N", "Newtons", dim(1, 1, -2), 1, "", {}, {"n", "newton", "newtons"});
        // Angle
        u("°", "Degrees", dim(0, 0, 0, 0, 0, 1), M_PI / 180, "rad", {"°"}, {"deg", "degree", "degrees"});
        u("rad", "Radians", dim(0, 0, 0, 0, 0, 1), 1, "°", {}, {"rad", "radian", "radians"});
        // Temperatures: absolute, converted but not composed.
        k.add({"°C", "Celsius", dim(), 1, 273.15, true, "", "°F"}, {"°C", "C"}, {"c", "°c", "celsius", "degc"});
        k.add({"°F", "Fahrenheit", dim(), 5.0 / 9, 459.67, true, "", "°C"}, {"°F", "F"}, {"f", "°f", "fahrenheit", "degf"});
        k.add({"K", "Kelvin", dim(), 1, 0, true, "", "°C"}, {"K"}, {"kelvin"});
        return k;
    }();
    return c;
}

// --- currencies -------------------------------------------------------------------------

struct CurrencyName {
    const char* word;
    const char* code;
};

// Names and signs; codes are the ECB's list.
constexpr CurrencyName kCurrencyWords[] = {
    {"$", "USD"}, {"€", "EUR"}, {"£", "GBP"}, {"¥", "JPY"}, {"₹", "INR"}, {"₩", "KRW"}, {"₺", "TRY"},
    {"₪", "ILS"}, {"₱", "PHP"}, {"฿", "THB"}, {"zł", "PLN"}, {"kč", "CZK"}, {"r$", "BRL"}, {"a$", "AUD"},
    {"c$", "CAD"}, {"nz$", "NZD"}, {"hk$", "HKD"}, {"s$", "SGD"},
    {"dollar", "USD"}, {"dollars", "USD"}, {"euro", "EUR"}, {"euros", "EUR"}, {"yen", "JPY"},
    {"rupee", "INR"}, {"rupees", "INR"}, {"franc", "CHF"}, {"francs", "CHF"}, {"yuan", "CNY"},
    {"renminbi", "CNY"}, {"rmb", "CNY"}, {"won", "KRW"}, {"peso", "MXN"}, {"pesos", "MXN"},
    {"real", "BRL"}, {"reais", "BRL"}, {"rand", "ZAR"}, {"krona", "SEK"}, {"kronor", "SEK"},
    {"krone", "NOK"}, {"kroner", "NOK"}, {"zloty", "PLN"}, {"forint", "HUF"}, {"lira", "TRY"},
    {"shekel", "ILS"}, {"shekels", "ILS"}, {"ringgit", "MYR"}, {"baht", "THB"}, {"rupiah", "IDR"},
    {"sterling", "GBP"}, {"quid", "GBP"},
};

std::optional<std::string> currency_code(const std::string& word, const Rates* rates) {
    if (!rates)
        return std::nullopt;
    const std::string l = lower(word);
    for (const CurrencyName& n : kCurrencyWords)
        if (l == n.word)
            return std::string(n.code);
    std::string up = l;
    for (char& c : up)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    if (up.size() == 3 && (up == "EUR" || rates->per_euro.contains(up)))
        return up;
    return std::nullopt;
}

// A money "unit": one of the code, in euros.
const Unit* currency_unit(const std::string& code, const Rates& rates) {
    static std::map<std::string, Unit> made;
    const double per = code == "EUR" ? 1.0 : rates.per_euro.contains(code) ? rates.per_euro.at(code) : 0;
    if (per <= 0)
        return nullptr;
    Unit& u = made[code];
    u = {code, code, dim(0, 0, 0, 0, 1), 1 / per, 0, false, code, ""};
    return &u;
}

// --- the expression grammar ------------------------------------------------------------

struct Token {
    enum Kind { Number, Word, Op, End } kind;
    double number = 0;
    std::string text;   // a word, or an operator's spelling
    bool based = false; // written in hex, binary or octal
    bool space_before = false;
};

std::vector<Token> tokenize(std::string_view s) {
    std::vector<Token> out;
    size_t i = 0;
    bool space = false;
    auto is_word = [](unsigned char c) { return std::isalpha(c) || c >= 0x80 || c == '_' || c == '$' || c == '\''; };
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isspace(c)) {
            space = true;
            ++i;
            continue;
        }
        Token t{Token::End};
        t.space_before = space;
        space = false;
        if (std::isdigit(c) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            if (c == '0' && i + 2 < s.size() && std::strchr("xXbBoO", s[i + 1]) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                const int base = std::tolower(s[i + 1]) == 'x' ? 16 : std::tolower(s[i + 1]) == 'b' ? 2 : 8;
                size_t j = i + 2;
                while (j < s.size() && (std::isxdigit(static_cast<unsigned char>(s[j])) || s[j] == '_'))
                    ++j;
                std::string digits(s.substr(i + 2, j - i - 2));
                std::erase(digits, '_');
                char* end = nullptr;
                t.number = double(std::strtoull(digits.c_str(), &end, base));
                if (*end)
                    throw std::runtime_error("digits");
                t.kind = Token::Number;
                t.based = true;
                i = j;
            } else {
                // 1,234.5 groups; a comma not followed by three digits ends the number.
                std::string digits;
                size_t j = i;
                while (j < s.size()) {
                    if (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.') {
                        digits += s[j++];
                    } else if (s[j] == ',' && j + 3 < s.size() + 1 && j + 3 <= s.size() &&
                               std::all_of(s.begin() + long(j) + 1, s.begin() + long(j) + 4,
                                           [](char d) { return std::isdigit(static_cast<unsigned char>(d)); }) &&
                               (j + 4 >= s.size() || !std::isdigit(static_cast<unsigned char>(s[j + 4])))) {
                        ++j;
                    } else if ((s[j] == 'e' || s[j] == 'E') && j + 1 < s.size() &&
                               (std::isdigit(static_cast<unsigned char>(s[j + 1])) ||
                                ((s[j + 1] == '-' || s[j + 1] == '+') && j + 2 < s.size() &&
                                 std::isdigit(static_cast<unsigned char>(s[j + 2]))))) {
                        digits += s[j++];
                        digits += s[j++];
                    } else {
                        break;
                    }
                }
                if (std::ranges::count(digits, '.') > 1)
                    throw std::runtime_error("dots");
                t.kind = Token::Number;
                t.number = std::strtod(digits.c_str(), nullptr);
                i = j;
            }
        } else if (is_word(c) && !s.substr(i).starts_with("×") && !s.substr(i).starts_with("÷") &&
                   !s.substr(i).starts_with("−")) {
            size_t j = i;
            // Digits go on a word that starts with a letter (m2, log2), not on a sign ($20).
            const bool lettered = std::isalpha(c);
            while (j < s.size() && (is_word(static_cast<unsigned char>(s[j])) ||
                                    (j > i && lettered && std::isdigit(static_cast<unsigned char>(s[j]))) ||
                                    (s[j] == '/' && j + 1 < s.size() && std::isalpha(static_cast<unsigned char>(s[j + 1])) &&
                                     j > i)))
                ++j;
            t.kind = Token::Word;
            t.text = std::string(s.substr(i, j - i));
            // "km/h" is one unit; "m/kg" is m over kg.
            if (t.text.find('/') != std::string::npos && !catalog().find(t.text)) {
                j = i + t.text.find('/');
                t.text = std::string(s.substr(i, j - i));
            }
            i = j;
        } else {
            t.kind = Token::Op;
            // ×, ÷ and − are operators too.
            if (s.substr(i).starts_with("×")) {
                t.text = "*";
                i += std::string_view("×").size();
            } else if (s.substr(i).starts_with("÷")) {
                t.text = "/";
                i += std::string_view("÷").size();
            } else if (s.substr(i).starts_with("−")) {
                t.text = "-";
                i += std::string_view("−").size();
            } else if (s.substr(i).starts_with("**")) {
                t.text = "^";
                i += 2;
            } else if (std::strchr("+-*/^()%,=!", c)) {
                t.text = std::string(1, char(c));
                ++i;
            } else {
                throw std::runtime_error("character");
            }
        }
        out.push_back(std::move(t));
    }
    out.push_back({Token::End});
    return out;
}

struct Value {
    double v = 0;               // in base units (temperatures: kelvin)
    Dim d{};
    const Unit* unit = nullptr; // the unit it was written in, for answers
    bool percent = false;
    bool based = false;         // a hex/binary/octal literal
    bool money() const { return d[C] != 0; }
};

struct Parse {
    const std::vector<Token>& t;
    size_t i;
    size_t end;  // parse t[i, end)
    const Context& ctx;
    int operators = 0;  // how many operators were applied: a lone quantity has none

    const Token& peek(size_t k = 0) const {
        static const Token end_token{Token::End};
        return i + k < end ? t[i + k] : end_token;
    }
    bool op(const char* s) const { return peek().kind == Token::Op && peek().text == s; }
    bool word(const char* s) const { return peek().kind == Token::Word && lower(peek().text) == s; }
    bool at_end() const { return i >= end; }

    const Unit* unit_word(const Token& tok) const {
        if (tok.kind != Token::Word)
            return nullptr;
        if (auto code = currency_code(tok.text, ctx.rates))
            return currency_unit(*code, *ctx.rates);
        return catalog().find(tok.text);
    }

    Value quantity(double n, const Unit* u) const {
        Value v;
        v.unit = u;
        v.d = u->dim;
        v.v = u->temperature ? (n + u->offset) * u->factor : n * u->factor;
        return v;
    }

    // expr := term (('+'|'-') term)*
    Value expr() {
        Value a = term();
        while (op("+") || op("-")) {
            const bool minus = peek().text == "-";
            ++i;
            Value b = term();
            a = add_sub(a, b, minus);
            ++operators;
        }
        return a;
    }

    Value add_sub(Value a, const Value& b, bool minus) const {
        if (b.percent && !a.percent) {
            // 80 + 15%: relative to what it's added to.
            a.v *= 1 + (minus ? -1 : 1) * b.v / 100;
            return a;
        }
        if (a.percent && b.percent) {
            a.v += (minus ? -1 : 1) * b.v;
            return a;
        }
        if (a.unit && a.unit->temperature)
            throw std::runtime_error("temperature arithmetic");
        // 5kg + 5: the bare number is in the unit beside it.
        Value bb = b;
        if (none(b.d) && !none(a.d) && a.unit && !b.unit)
            bb.v *= a.unit->factor, bb.d = a.d, bb.unit = a.unit;
        Value aa = a;
        if (none(a.d) && !none(b.d) && b.unit && !a.unit)
            aa.v *= b.unit->factor, aa.d = b.d;
        if (aa.d != bb.d)
            throw std::runtime_error("dimensions");
        Value r = aa;
        r.v = aa.v + (minus ? -1 : 1) * bb.v;
        // The last unit typed is the one the answer is in.
        r.unit = bb.unit ? bb.unit : aa.unit;
        r.percent = false;
        r.based = a.based && b.based;
        return r;
    }

    // term := power (('*'|'/'|'mod'|implicit) power)*
    Value term() {
        Value a = power();
        for (;;) {
            if (op("*") || op("/") || word("x") || word("mod") || word("times") || word("per")) {
                const std::string o = peek().kind == Token::Op ? peek().text : lower(peek().text);
                ++i;
                Value b = power();
                a = mul_div(a, b, o == "*" || o == "x" || o == "times" ? '*' : o == "mod" ? '%' : '/');
                ++operators;
            } else if (op("(")) {
                Value b = power();  // 2(3 + 4)
                a = mul_div(a, b, '*');
                ++operators;
            } else {
                return a;
            }
        }
    }

    Value mul_div(Value a, Value b, char o) const {
        if ((a.unit && a.unit->temperature) || (b.unit && b.unit->temperature))
            throw std::runtime_error("temperature arithmetic");
        if (a.percent && !b.percent)
            a.v /= 100, a.percent = false;
        if (b.percent)
            b.v /= 100, b.percent = false;
        Value r;
        if (o == '*') {
            r.v = a.v * b.v;
            r.d = add(a.d, b.d);
        } else if (o == '/') {
            if (b.v == 0)
                throw std::runtime_error("division by zero");
            r.v = a.v / b.v;
            r.d = add(a.d, b.d, -1);
        } else {
            if (b.d != a.d && !none(b.d))
                throw std::runtime_error("mod");
            r.v = std::fmod(a.v, b.v);
            r.d = a.d;
        }
        // A scalar keeps the other's unit; a quantity over its like is a scalar.
        if (r.d == a.d && a.unit && none(b.d))
            r.unit = a.unit;
        else if (r.d == b.d && b.unit && none(a.d))
            r.unit = b.unit;
        r.based = a.based && b.based;
        return r;
    }

    // power := unary ('^' power)?
    Value power() {
        Value a = unary();
        if (op("^")) {
            ++i;
            Value b = power();
            if (!none(b.d))
                throw std::runtime_error("exponent");
            if (!none(a.d)) {
                const double k = b.v;
                if (k != std::round(k))
                    throw std::runtime_error("exponent");
                a.d = scale(a.d, int(k));
                a.unit = nullptr;
            }
            a.v = std::pow(a.v, b.v);
            ++operators;
        }
        return a;
    }

    Value unary() {
        if (op("-")) {
            ++i;
            Value v = unary();
            v.v = v.unit && v.unit->temperature ? v.unit->factor * (-(v.v / v.unit->factor - v.unit->offset) + v.unit->offset) : -v.v;
            return v;
        }
        if (op("+")) {
            ++i;
            return unary();
        }
        return postfix();
    }

    // A primary with what follows it: a unit, a percent sign, "of", a
    // factorial, or another quantity in composite notation (5 ft 3 in).
    Value postfix() {
        Value v = primary();
        if (op("!")) {
            ++i;
            if (v.v < 0 || v.v != std::round(v.v) || v.v > 170 || !none(v.d))
                throw std::runtime_error("factorial");
            v.v = std::tgamma(v.v + 1);
            ++operators;
        }
        if (op("%")) {
            ++i;
            v.percent = true;
            // 15% of 80, 15% off 80, 15% on 80
            if (word("of") || word("off") || word("on")) {
                const std::string w = lower(peek().text);
                ++i;
                Value base = term();
                const double f = v.v / 100;
                base.v *= w == "of" ? f : w == "off" ? 1 - f : 1 + f;
                ++operators;
                return base;
            }
            return v;
        }
        return v;
    }

    Value primary() {
        const Token& tok = peek();
        if (op("(")) {
            ++i;
            Value v = expr();
            if (!op(")"))
                throw std::runtime_error("paren");
            ++i;
            return with_unit(v);
        }
        // $20, €5: the sign before the number.
        // $20, €5, EUR 20: a sign or a code before the number.
        if (tok.kind == Token::Word && peek(1).kind == Token::Number)
            if (auto code = currency_code(tok.text, ctx.rates);
                code && (!std::isalpha(static_cast<unsigned char>(tok.text[0])) || tok.text.find('$') != std::string::npos ||
                         tok.text.size() == 3)) {
                ++i;
                const double n = peek().number;
                ++i;
                return composite(quantity(n, currency_unit(*code, *ctx.rates)));
            }
        if (tok.kind == Token::Number) {
            ++i;
            Value v;
            v.v = tok.number;
            v.based = tok.based;
            return with_unit(v);
        }
        if (tok.kind == Token::Word) {
            const std::string w = lower(tok.text);
            if (w == "pi" || w == "π") {
                ++i;
                return {M_PI};
            }
            if (w == "e" && !(peek(1).kind == Token::Number)) {
                ++i;
                return {M_E};
            }
            static const std::map<std::string, std::function<double(double)>> functions = {
                {"sqrt", [](double x) { return std::sqrt(x); }}, {"cbrt", [](double x) { return std::cbrt(x); }},
                {"sin", [](double x) { return std::sin(x); }},   {"cos", [](double x) { return std::cos(x); }},
                {"tan", [](double x) { return std::tan(x); }},   {"asin", [](double x) { return std::asin(x); }},
                {"acos", [](double x) { return std::acos(x); }}, {"atan", [](double x) { return std::atan(x); }},
                {"log", [](double x) { return std::log10(x); }}, {"ln", [](double x) { return std::log(x); }},
                {"log2", [](double x) { return std::log2(x); }}, {"exp", [](double x) { return std::exp(x); }},
                {"abs", [](double x) { return std::fabs(x); }},  {"round", [](double x) { return std::round(x); }},
                {"floor", [](double x) { return std::floor(x); }}, {"ceil", [](double x) { return std::ceil(x); }},
            };
            if (auto f = functions.find(w);
                f != functions.end() && (peek(1).kind == Token::Number || (peek(1).kind == Token::Op && peek(1).text == "("))) {
                ++i;
                Value arg = unary_arg();
                ++operators;
                if (w == "sqrt" || w == "cbrt") {
                    const int k = w == "sqrt" ? 2 : 3;
                    for (int x : arg.d)
                        if (x % k)
                            throw std::runtime_error("root");
                    Dim d = arg.d;
                    for (int& x : d)
                        x /= k;
                    Value r{f->second(arg.v), d};
                    return r;
                }
                if (w == "abs" || w == "round" || w == "floor" || w == "ceil") {
                    if (arg.unit) {
                        const double in_unit = arg.v / arg.unit->factor;
                        arg.v = f->second(in_unit) * arg.unit->factor;
                    } else {
                        arg.v = f->second(arg.v);
                    }
                    return arg;
                }
                // Trigonometry takes radians, or an angle.
                if (!none(arg.d) && arg.d != dim(0, 0, 0, 0, 0, 1))
                    throw std::runtime_error("function of a quantity");
                return {f->second(arg.v)};
            }
            // "square root of 25"
            if (w == "square" && peek(1).kind == Token::Word && lower(peek(1).text) == "root") {
                i += 2;
                if (word("of"))
                    ++i;
                Value a = unary();
                ++operators;
                return {std::sqrt(a.v), a.d};
            }
        }
        throw std::runtime_error("expected a value");
    }

    Value unary_arg() {
        if (op("(")) {
            ++i;
            Value v = expr();
            if (!op(")"))
                throw std::runtime_error("paren");
            ++i;
            return v;
        }
        return unary();
    }

    // A number followed by its unit: "5 kg", "5kg", "20 dollars".
    Value with_unit(Value v) {
        if (!none(v.d) || v.unit)
            return v;
        const Token& tok = peek();
        if (tok.kind != Token::Word)
            return v;
        // "to"/"in" before a unit is a conversion, handled above; a lone
        // "in" after a number is inches.
        const Unit* u = unit_word(tok);
        if (!u)
            return v;
        ++i;
        return composite(quantity(v.v, u));
    }

    // 5 ft 3 in, 1 hr 30 min: one quantity, in the leading unit.
    Value composite(Value v) {
        while (peek().kind == Token::Number && peek(1).kind == Token::Word && v.unit && !v.unit->temperature) {
            const Unit* u = unit_word(peek(1));
            if (!u || u->dim != v.d || u == v.unit)
                break;
            const double n = peek().number;
            i += 2;
            v.v += n * u->factor;
            ++operators;
        }
        return v;
    }
};

// A conversion target: a unit ("km/h", "celsius", "eur") or a base.
struct Target {
    const Unit* unit = nullptr;
    int base = 0;  // 2, 8, 10, 16
};

std::optional<Target> parse_target(const std::vector<Token>& t, size_t from, size_t end, const Context& ctx) {
    if (from >= end)
        return std::nullopt;
    std::string word;
    for (size_t k = from; k < end; ++k) {
        if (t[k].kind == Token::Op && t[k].text == "/" && !word.empty()) {
            word += "/";
            continue;
        }
        if (t[k].kind != Token::Word)
            return std::nullopt;
        word += (word.empty() || word.back() == '/' ? "" : " ") + t[k].text;
    }
    const std::string l = lower(word);
    static const std::map<std::string, int> bases = {{"hex", 16}, {"hexadecimal", 16}, {"bin", 2}, {"binary", 2},
                                                     {"oct", 8}, {"octal", 8}, {"dec", 10}, {"decimal", 10}};
    if (auto b = bases.find(l); b != bases.end())
        return Target{nullptr, b->second};
    if (auto code = currency_code(word, ctx.rates))
        return Target{currency_unit(*code, *ctx.rates), 0};
    if (const Unit* u = catalog().find(word))
        return Target{u, 0};
    // "fl oz", "nautical miles": two words as one.
    std::string joined = l;
    std::erase(joined, ' ');
    if (const Unit* u = catalog().find(joined))
        return Target{u, 0};
    return std::nullopt;
}

double in_unit(const Value& v, const Unit* u) {
    return u->temperature ? v.v / u->factor - u->offset : v.v / u->factor;
}

std::string money(double v, const std::string& code) {
    return format_fixed(v, 2) + " " + code;
}

std::string show(double n, const Unit* u) {
    if (!u->currency.empty())
        return money(n, u->currency);
    return format(n) + (u->symbol == "°" || u->symbol.starts_with("°") ? "" : " ") + u->symbol;
}

std::string plain(double n) {
    std::string s = format(n, 15);
    std::erase(s, ',');
    return s;
}

std::string in_base(double v, int base) {
    if (v != std::round(v) || std::fabs(v) > 9.0e15)
        throw std::runtime_error("base");
    long long n = (long long)v;
    const bool neg = n < 0;
    unsigned long long u = (unsigned long long)(neg ? -n : n);
    std::string digits;
    do {
        digits.insert(digits.begin(), "0123456789ABCDEF"[u % unsigned(base)]);
        u /= unsigned(base);
    } while (u);
    const char* prefix = base == 16 ? "0x" : base == 2 ? "0b" : base == 8 ? "0o" : "";
    return (neg ? "-" : "") + std::string(prefix) + digits;
}

// The unit an answer with no unit of its own is shown in.
const Unit* base_unit_for(const Dim& d) {
    static const std::pair<Dim, const char*> bases[] = {
        {dim(1), "m"},       {dim(0, 1), "kg"},        {dim(0, 0, 1), "s"},     {dim(0, 0, 0, 1), "B"},
        {dim(2), "m2"},      {dim(3), "m3"},           {dim(1, 0, -1), "m/s"},  {dim(0, 0, -1), "hz"},
        {dim(0, 0, -1, 1), "b/s"}, {dim(2, 1, -2), "j"}, {dim(2, 1, -3), "w"}, {dim(-1, 1, -2), "pa"},
        {dim(1, 1, -2), "n"}, {dim(0, 0, 0, 0, 0, 1), "rad"},
    };
    for (const auto& [bd, name] : bases)
        if (bd == d)
            return catalog().find(name);
    return nullptr;
}

std::optional<Answer> expression(std::string_view query, const Context& ctx) {
    const std::vector<Token> t = tokenize(query);
    const size_t end = t.size() - 1;  // the End token
    if (end == 0)
        return std::nullopt;
    // Single words are searches, not sums.
    if (end == 1 && t[0].kind == Token::Word)
        return std::nullopt;

    // Rightmost "to"/"in"/"as"/"=" at depth 0 whose right side is a target.
    std::optional<Target> target;
    size_t split = end;
    int depth = 0;
    for (size_t k = end; k-- > 1;) {
        if (t[k].kind == Token::Op && t[k].text == ")")
            ++depth;
        if (t[k].kind == Token::Op && t[k].text == "(")
            --depth;
        if (depth != 0)
            continue;
        const bool kw = (t[k].kind == Token::Word &&
                         (lower(t[k].text) == "to" || lower(t[k].text) == "in" || lower(t[k].text) == "as" ||
                          lower(t[k].text) == "into")) ||
                        (t[k].kind == Token::Op && t[k].text == "=");
        if (!kw)
            continue;
        if (auto tg = parse_target(t, k + 1, end, ctx)) {
            target = tg;
            split = k;
            break;
        }
    }
    // "255 to hex" alone reads the target too.
    Parse p{t, 0, split, ctx};
    Value v = p.expr();
    if (!p.at_end())
        throw std::runtime_error("trailing input");

    Answer a;
    a.input = trim(query);
    if (target) {
        if (target->base) {
            if (!none(v.d))
                return std::nullopt;
            a.result = target->base == 10 ? format(v.v) : in_base(v.v, target->base);
            a.copy = target->base == 10 ? plain(v.v) : a.result;
            a.result_badge = target->base == 16 ? "Hexadecimal" : target->base == 2 ? "Binary" : target->base == 8 ? "Octal" : "Decimal";
            a.input_badge = v.based ? "Number" : "Decimal";
            return a;
        }
        const Unit* to = target->unit;
        // A bare number converts as if written in the target's system: "5 to cm" means nothing.
        if (v.d != to->dim || (to->temperature != (v.unit && v.unit->temperature)))
            return std::nullopt;
        const double n = in_unit(v, to);
        a.result = show(n, to);
        a.copy = plain(n);
        a.result_badge = to->name;
        a.input_badge = v.unit ? v.unit->name : "Result";
        return a;
    }

    // A lone number (or percentage) isn't a calculation; a based literal is.
    if (p.operators == 0 && none(v.d) && !v.unit) {
        if (!v.based)
            return std::nullopt;
        a.result = format(v.v);
        a.copy = plain(v.v);
        a.result_badge = "Decimal";
        const std::string l = lower(query);
        a.input_badge = l.find("0x") != std::string::npos ? "Hexadecimal" : l.find("0b") != std::string::npos ? "Binary" : "Octal";
        return a;
    }

    // A lone quantity shows in its partner unit ("5 ft" → metres; "20 eur" → home).
    if (p.operators == 0 && v.unit) {
        const Unit* partner = nullptr;
        if (!v.unit->currency.empty()) {
            if (!ctx.rates)
                return std::nullopt;
            const std::string code = v.unit->currency == ctx.home_currency ? (ctx.home_currency == "EUR" ? "USD" : "EUR")
                                                                           : ctx.home_currency;
            partner = currency_unit(code, *ctx.rates);
        } else if (!v.unit->partner.empty()) {
            partner = catalog().find(v.unit->partner);
        }
        if (!partner)
            return std::nullopt;
        const double n = in_unit(v, partner);
        a.result = show(n, partner);
        a.copy = plain(n);
        a.result_badge = partner->name;
        a.input_badge = v.unit->name;
        return a;
    }

    if (v.percent) {
        a.result = format(v.v) + "%";
        a.copy = plain(v.v) + "%";
        a.result_badge = "Percent";
        return a;
    }
    if (none(v.d)) {
        a.result = format(v.v);
        a.copy = plain(v.v);
        a.result_badge = "Result";
        return a;
    }
    const Unit* u = v.unit && v.unit->dim == v.d ? v.unit : base_unit_for(v.d);
    if (!u)
        return std::nullopt;
    const double n = in_unit(v, u);
    a.result = show(n, u);
    a.copy = plain(n);
    a.result_badge = u->name;
    return a;
}

// --- dates and time zones -----------------------------------------------------------------

const char* const kMonths[] = {"january", "february", "march", "april", "may", "june", "july",
                               "august", "september", "october", "november", "december"};
const char* const kWeekdays[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};

std::string title(std::string s) {
    if (!s.empty())
        s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!cur.empty())
                out.push_back(cur), cur.clear();
        } else {
            cur += char(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

int month_of(const std::string& w) {
    for (int m = 0; m < 12; ++m)
        if (w.size() >= 3 && std::string_view(kMonths[m]).starts_with(w) && (w.size() == 3 || w == kMonths[m] || w == "sept"))
            return m + 1;
    return 0;
}

int weekday_of(const std::string& w) {
    for (int d = 0; d < 7; ++d)
        if (w.size() >= 3 && std::string_view(kWeekdays[d]).starts_with(w))
            return d;
    return -1;
}

std::optional<int> number(const std::string& w) {
    std::string s = w;
    // 25th, 1st, 2nd, 3rd; a trailing dot as in "25."
    for (const char* suffix : {"st", "nd", "rd", "th", "."})
        if (s.size() > std::strlen(suffix) && s.ends_with(suffix) && std::isdigit(static_cast<unsigned char>(s[s.size() - std::strlen(suffix) - 1])))
            s.resize(s.size() - std::strlen(suffix));
    if (s.empty() || !std::ranges::all_of(s, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        return std::nullopt;
    return std::atoi(s.c_str());
}

struct Clock {
    int minutes;  // since midnight
};

// "9am", "9:30pm", "21:00", "noon", "midnight", or "9 am" as two words.
std::optional<Clock> clock_of(const std::vector<std::string>& w, size_t& k) {
    if (k >= w.size())
        return std::nullopt;
    std::string s = w[k];
    size_t used = 1;
    if (k + 1 < w.size() && (w[k + 1] == "am" || w[k + 1] == "pm") && number(s))
        s += w[k + 1], used = 2;
    if (s == "noon" || s == "midday") {
        k += used;
        return Clock{12 * 60};
    }
    if (s == "midnight") {
        k += used;
        return Clock{0};
    }
    std::string suffix;
    if (s.ends_with("am") || s.ends_with("pm")) {
        suffix = s.substr(s.size() - 2);
        s.resize(s.size() - 2);
    }
    int h = 0, m = 0;
    const size_t colon = s.find(':');
    if (colon != std::string::npos) {
        auto hh = number(s.substr(0, colon)), mm = number(s.substr(colon + 1));
        if (!hh || !mm || s.size() - colon - 1 != 2)
            return std::nullopt;
        h = *hh, m = *mm;
    } else {
        if (suffix.empty())
            return std::nullopt;  // a bare number is a number
        auto hh = number(s);
        if (!hh)
            return std::nullopt;
        h = *hh;
    }
    if (!suffix.empty()) {
        if (h < 1 || h > 12)
            return std::nullopt;
        h = h % 12 + (suffix == "pm" ? 12 : 0);
    }
    if (h > 23 || m > 59)
        return std::nullopt;
    k += used;
    return Clock{h * 60 + m};
}

using Zone = const time_zone*;

Zone zone_of(const Context& ctx) {
    try {
        return ctx.zone.empty() ? current_zone() : locate_zone(ctx.zone);
    } catch (...) {
        return locate_zone("UTC");
    }
}

local_days today(const Context& ctx) {
    return floor<days>(zone_of(ctx)->to_local(ctx.now));
}

// A day written out: "25 dec", "dec 25", "25 december 2026", "2026-12-25",
// "25.12.2026" (day first), "12/25/2026" (month first), today, tomorrow,
// yesterday, next friday, friday. `bias` +1 looks ahead for a date without a
// year, -1 behind, 0 the nearest.
std::optional<local_days> date_of(const std::vector<std::string>& w, size_t& k, const Context& ctx, int bias) {
    if (k >= w.size())
        return std::nullopt;
    const local_days now = today(ctx);
    const year_month_day ymd{now};
    const std::string& s = w[k];
    if (s == "today") return ++k, now;
    if (s == "tomorrow") return ++k, now + days(1);
    if (s == "yesterday") return ++k, now - days(1);
    // next friday, last friday, friday
    if ((s == "next" || s == "last" || s == "this") && k + 1 < w.size() && weekday_of(w[k + 1]) >= 0) {
        const int want = weekday_of(w[k + 1]);
        const int have = int(weekday{now}.c_encoding());
        k += 2;
        int delta = (want - have + 7) % 7;
        if (s == "next")
            return now + days(delta == 0 ? 7 : delta);
        if (s == "last")
            return now - days((have - want + 7) % 7 == 0 ? 7 : (have - want + 7) % 7);
        return now + days(delta);
    }
    if (weekday_of(s) >= 0 && s.size() >= 3) {
        const int want = weekday_of(s), have = int(weekday{now}.c_encoding());
        ++k;
        if (bias < 0)
            return now - days((have - want + 7) % 7);
        return now + days((want - have + 7) % 7);
    }
    auto finish = [&](int y, int m, int d, bool has_year) -> std::optional<local_days> {
        if (y < 100 && has_year)
            y += y <= 68 ? 2000 : 1900;
        auto make = [&](int yy) { return year_month_day{year(yy), month(unsigned(m)), day(unsigned(d))}; };
        if (!make(has_year ? y : int(ymd.year())).ok())
            return std::nullopt;
        if (has_year)
            return local_days{make(y)};
        // No year: the one it is nearest (or ahead of / behind today, by bias).
        const int this_year = int(ymd.year());
        const local_days a{make(this_year)};
        if (bias > 0)
            return a >= now ? a : local_days{make(this_year + 1)};
        if (bias < 0)
            return a <= now ? a : local_days{make(this_year - 1)};
        const local_days before{make(this_year - 1)}, after{make(this_year + 1)};
        auto dist = [&](local_days x) { return std::abs((x - now).count()); };
        return dist(a) <= dist(before) && dist(a) <= dist(after) ? a : dist(before) < dist(after) ? before : after;
    };
    // Numeric forms.
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3 && s.find('-') == 4)
        return ++k, finish(y, m, d, true);
    if (std::ranges::count(s, '.') == 2 && std::sscanf(s.c_str(), "%d.%d.%d", &d, &m, &y) == 3) {
        const std::string tail = s.substr(s.rfind('.') + 1);
        if (tail.size() == 2 || tail.size() == 4)
            return ++k, finish(y, m, d, true);
        return std::nullopt;
    }
    if (std::ranges::count(s, '/') == 2 && std::sscanf(s.c_str(), "%d/%d/%d", &m, &d, &y) == 3)
        return ++k, finish(y, m, d, true);
    // 25 dec [2026], dec 25 [2026]
    auto year_after = [&](size_t at, bool& has) -> int {
        if (at < w.size())
            if (auto n = number(w[at]); n && w[at].size() == 4) {
                has = true;
                return *n;
            }
        return int(ymd.year());
    };
    if (auto n = number(s); n && k + 1 < w.size() && month_of(w[k + 1])) {
        bool has = false;
        const int yy = year_after(k + 2, has);
        const int mm = month_of(w[k + 1]);
        k += has ? 3 : 2;
        return finish(yy, mm, *n, has);
    }
    if (month_of(s) && k + 1 < w.size())
        if (auto n = number(w[k + 1])) {
            bool has = false;
            const int yy = year_after(k + 2, has);
            const int mm = month_of(s);
            k += has ? 3 : 2;
            return finish(yy, mm, *n, has);
        }
    return std::nullopt;
}

std::string date_text(local_days d, const Context& ctx) {
    const year_month_day ymd{d};
    std::string s = std::to_string(unsigned(ymd.day())) + " " + title(kMonths[unsigned(ymd.month()) - 1]);
    if (ymd.year() != year_month_day{today(ctx)}.year())
        s += " " + std::to_string(int(ymd.year()));
    return s;
}

std::string weekday_text(local_days d) {
    return title(kWeekdays[weekday{d}.c_encoding()]);
}

std::string clock_text(int minutes) {
    const int h = minutes / 60 % 24, m = minutes % 60;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
    return buf;
}

struct Span {
    long long count;
    const char* unit;  // days, weeks, hours, minutes, months, years
};

// "3 weeks", "90 min", "1 day"
std::optional<Span> span_of(const std::vector<std::string>& w, size_t& k) {
    if (k + 1 >= w.size())
        return std::nullopt;
    auto n = number(w[k]);
    if (!n)
        return std::nullopt;
    const std::string& u = w[k + 1];
    const char* unit = nullptr;
    if (u == "d" || u.starts_with("day")) unit = "days";
    else if (u == "w" || u.starts_with("week") || u == "wk" || u == "wks") unit = "weeks";
    else if (u == "h" || u.starts_with("hour") || u == "hr" || u == "hrs") unit = "hours";
    else if (u.starts_with("min")) unit = "minutes";
    else if (u.starts_with("month") || u == "mo") unit = "months";
    else if (u.starts_with("year") || u == "y" || u == "yr" || u == "yrs") unit = "years";
    else if (u.starts_with("weekday")) unit = "weekdays";
    if (!unit)
        return std::nullopt;
    k += 2;
    return Span{*n, unit};
}

// A moment shifted by a span, in local time (minutes since the local epoch).
local_time<minutes> shift(local_time<minutes> t, Span s) {
    const auto day_part = floor<days>(t);
    const minutes clock = t - day_part;
    if (std::string_view(s.unit) == "minutes") return t + minutes(s.count);
    if (std::string_view(s.unit) == "hours") return t + hours(s.count);
    if (std::string_view(s.unit) == "days") return t + days(s.count);
    if (std::string_view(s.unit) == "weeks") return t + weeks(s.count);
    if (std::string_view(s.unit) == "weekdays") {
        local_days d = day_part;
        long long left = std::llabs(s.count);
        const int step = s.count < 0 ? -1 : 1;
        while (left > 0) {
            d += days(step);
            const unsigned wd = weekday{d}.c_encoding();
            if (wd != 0 && wd != 6)
                --left;
        }
        return d + clock;
    }
    // Months and years clamp to the month's end: 31 Jan + 1 month = 28 Feb.
    year_month_day ymd{day_part};
    const long long months_n = std::string_view(s.unit) == "years" ? s.count * 12 : s.count;
    year_month ym = year_month{ymd.year(), ymd.month()} + months(months_n);
    const day last = year_month_day_last{ym.year(), month_day_last{ym.month()}}.day();
    const year_month_day r{ym.year(), ym.month(), std::min(ymd.day(), last)};
    return local_days{r} + clock;
}

std::optional<Answer> date_answer(local_days d, const std::string& input, const Context& ctx) {
    Answer a;
    a.input = input;
    a.input_badge = "Date";
    a.result = date_text(d, ctx);
    a.result_badge = weekday_text(d);
    const year_month_day ymd{d};
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", int(ymd.year()), unsigned(ymd.month()), unsigned(ymd.day()));
    a.copy = buf;
    return a;
}

std::string duration_text(long long total_minutes) {
    const long long m = std::llabs(total_minutes);
    std::string s;
    if (m >= 1440 && m % 1440 == 0)
        s = format(double(m / 1440)) + (m / 1440 == 1 ? " day" : " days");
    else if (m >= 60)
        s = format(double(m / 60)) + " hr" + (m % 60 ? " " + std::to_string(m % 60) + " min" : "");
    else
        s = std::to_string(m) + " min";
    return (total_minutes < 0 ? "-" : "") + s;
}

std::optional<Answer> dates(std::string_view query, const Context& ctx) {
    const std::vector<std::string> w = words(query);
    if (w.empty())
        return std::nullopt;
    const Zone zone = zone_of(ctx);
    const local_time<minutes> now = floor<minutes>(zone->to_local(ctx.now));
    const std::string input = trim(query);

    // "days until 25 dec", "hours till 9pm", "weeks since 1 jan"
    if (w.size() >= 3 && (w[1] == "until" || w[1] == "till" || w[1] == "til" || w[1] == "to" || w[1] == "since")) {
        const std::string& u = w[0];
        const bool since = w[1] == "since";
        size_t k = 2;
        std::optional<local_time<minutes>> when;
        if (auto c = clock_of(w, k); c && k == w.size()) {
            local_time<minutes> t = floor<days>(now) + minutes(c->minutes);
            if (!since && t <= now) t += days(1);
            if (since && t > now) t -= days(1);
            when = t;
        } else {
            k = 2;
            if (auto d = date_of(w, k, ctx, since ? -1 : 1)) {
                local_time<minutes> t = *d + minutes(0);
                if (k < w.size() && w[k] == "at") {
                    ++k;
                    auto c = clock_of(w, k);
                    if (!c)
                        return std::nullopt;
                    t = *d + minutes(c->minutes);
                }
                if (k != w.size())
                    return std::nullopt;
                when = t;
            }
        }
        if (!when)
            return std::nullopt;
        const long long mins = (since ? now - *when : *when - now).count();
        // Days count calendar days when both ends are dates.
        Answer a;
        a.input = input;
        a.input_badge = since ? "Since" : "Until";
        double value = 0;
        const char* name = nullptr;
        if (u.starts_with("day")) {
            value = double((since ? floor<days>(now) - floor<days>(*when) : floor<days>(*when) - floor<days>(now)).count());
            name = "days";
        } else if (u.starts_with("week")) {
            value = double(mins) / (7 * 1440), name = "weeks";
        } else if (u.starts_with("hour") || u == "hrs" || u == "hr" || u == "h") {
            value = double(mins) / 60, name = "hours";
        } else if (u.starts_with("min")) {
            value = double(mins), name = "minutes";
        } else if (u.starts_with("month")) {
            value = double(mins) / (1440 * 30.436875), name = "months";
        } else if (u.starts_with("year")) {
            value = double(mins) / (1440 * 365.2425), name = "years";
        } else if (u == "time") {
            a.result = duration_text(mins);
            a.result_badge = "Duration";
            a.copy = a.result;
            return a;
        } else {
            return std::nullopt;
        }
        const std::string v = format(std::round(value * 100) / 100);
        a.result = v + " " + (v == "1" ? std::string(name).substr(0, std::strlen(name) - 1) : std::string(name));
        a.result_badge = title(name);
        a.copy = plain(std::round(value * 100) / 100);
        return a;
    }

    // "3 days from now", "2 weeks ago", "5 weekdays from today"
    {
        size_t k = 0;
        if (auto s = span_of(w, k)) {
            if (k + 2 == w.size() && w[k] == "from" && (w[k + 1] == "now" || w[k + 1] == "today")) {
                if (w[k + 1] == "today" || (std::string_view(s->unit) != "hours" && std::string_view(s->unit) != "minutes"))
                    return date_answer(floor<days>(shift(floor<days>(now) + minutes(0), *s)), input, ctx);
                const auto t = shift(now, *s);
                Answer a = *date_answer(floor<days>(t), input, ctx);
                a.result = clock_text(int((t - floor<days>(t)).count())) + ", " + a.result;
                return a;
            }
            if (k + 1 == w.size() && w[k] == "ago") {
                s->count = -s->count;
                if (std::string_view(s->unit) == "hours" || std::string_view(s->unit) == "minutes") {
                    const auto t = shift(now, *s);
                    Answer a = *date_answer(floor<days>(t), input, ctx);
                    a.result = clock_text(int((t - floor<days>(t)).count())) + ", " + a.result;
                    return a;
                }
                return date_answer(floor<days>(shift(floor<days>(now) + minutes(0), *s)), input, ctx);
            }
        }
    }

    // "today + 3 weeks", "now - 90 min", "25 dec + 10 days", "9pm + 3 hours"
    {
        size_t k = 0;
        bool timed = false;
        local_time<minutes> t;
        if (w[0] == "now") {
            t = now, timed = true, k = 1;
        } else if (auto c = clock_of(w, k)) {
            t = floor<days>(now) + minutes(c->minutes), timed = true;
        } else if (auto d = date_of(w, k = 0, ctx, 0)) {
            t = *d + minutes(0);
            if (k < w.size() && w[k] == "at") {
                ++k;
                auto c = clock_of(w, k);
                if (!c)
                    return std::nullopt;
                t += minutes(c->minutes), timed = true;
            }
        } else {
            return std::nullopt;
        }
        const size_t moment_end = k;
        bool shifted = false;
        while (k < w.size()) {
            // "+ 3 weeks", "+3 weeks"
            std::string sign = w[k];
            std::vector<std::string> rest(w.begin() + long(k), w.end());
            if (sign == "+" || sign == "-") {
                ++k;
            } else if ((sign.starts_with('+') || sign.starts_with('-')) && sign.size() > 1) {
                rest[0] = sign.substr(1);
                std::vector<std::string> w2(w.begin(), w.begin() + long(k));
                w2.push_back(sign.substr(0, 1));
                w2.insert(w2.end(), rest.begin(), rest.end());
                return dates([&] {
                    std::string q;
                    for (const auto& x : w2)
                        q += x + " ";
                    return q;
                }(), ctx);
            } else {
                return std::nullopt;
            }
            size_t j = k;
            auto s = span_of(w, j);
            if (!s) {
                // A bare number: hours off a clock, days off a date.
                if (j < w.size())
                    if (auto n = number(w[j])) {
                        s = Span{*n, timed ? "hours" : "days"};
                        j += 1;
                    }
            }
            if (!s)
                return std::nullopt;
            if (sign.starts_with('-'))
                s->count = -s->count;
            if (std::string_view(s->unit) == "hours" || std::string_view(s->unit) == "minutes")
                timed = true;
            t = shift(t, *s);
            k = j;
            shifted = true;
        }
        // A day written out on its own ("25 dec", "next friday") answers with its weekday.
        if (!shifted) {
            if (timed || w[0] == "today" || moment_end != w.size())
                return std::nullopt;
            const bool written = w.size() >= 2 || w[0].find_first_of("-./") != std::string::npos;
            if (!written)
                return std::nullopt;  // "friday", "tomorrow": searches
            return date_answer(floor<days>(t), input, ctx);
        }
        Answer a = *date_answer(floor<days>(t), input, ctx);
        if (timed)
            a.result = clock_text(int((t - floor<days>(t)).count())) + ", " + a.result;
        return a;
    }
}

// --- time in a place ----------------------------------------------------------------------

// A place's zone: a city in the tz database ("tokyo", "new york", "sao paulo"),
// a zone's own name, UTC/GMT, or a few everyday short names.
Zone place_zone(const std::string& place) {
    static const std::map<std::string, std::string> shorthands = {
        {"sf", "America/Los_Angeles"}, {"san francisco", "America/Los_Angeles"}, {"la", "America/Los_Angeles"},
        {"nyc", "America/New_York"},   {"ny", "America/New_York"},             {"london", "Europe/London"},
        {"ldn", "Europe/London"},      {"utc", "UTC"},                          {"gmt", "Etc/GMT"},
        {"uk", "Europe/London"},       {"japan", "Asia/Tokyo"},                 {"india", "Asia/Kolkata"},
        {"china", "Asia/Shanghai"},    {"beijing", "Asia/Shanghai"},            {"delhi", "Asia/Kolkata"},
        {"mumbai", "Asia/Kolkata"},    {"germany", "Europe/Berlin"},            {"france", "Europe/Paris"},
        {"seattle", "America/Los_Angeles"}, {"boston", "America/New_York"},     {"washington", "America/New_York"},
        {"texas", "America/Chicago"},  {"dallas", "America/Chicago"},           {"houston", "America/Chicago"},
        {"miami", "America/New_York"}, {"atlanta", "America/New_York"},         {"austin", "America/Chicago"},
        {"sydney", "Australia/Sydney"}, {"korea", "Asia/Seoul"},               {"serbia", "Europe/Belgrade"},
    };
    std::string p = lower(trim(place));
    if (auto it = shorthands.find(p); it != shorthands.end())
        return locate_zone(it->second);
    std::string under = p;
    std::ranges::replace(under, ' ', '_');
    try {
        const tzdb& db = get_tzdb();
        for (const time_zone& z : db.zones) {
            const std::string name = lower(z.name());
            if (name == under)
                return &z;
            const size_t slash = name.rfind('/');
            if (slash != std::string::npos && name.substr(slash + 1) == under && !name.starts_with("etc/"))
                return &z;
        }
    } catch (...) {
    }
    return nullptr;
}

std::string place_name(const time_zone* z, const std::string& typed) {
    std::string n(z->name());
    const size_t slash = n.rfind('/');
    n = slash == std::string::npos ? n : n.substr(slash + 1);
    std::ranges::replace(n, '_', ' ');
    if (n == "GMT" || n == "UTC")
        return n;
    // The words as typed read better than the zone's city for a country or a shorthand.
    std::string t = trim(typed);
    if (t.size() > 3) {
        std::string out;
        bool up = true;
        for (char c : t) {
            out += up ? char(std::toupper(static_cast<unsigned char>(c))) : c;
            up = c == ' ';
        }
        return out;
    }
    return n;
}

std::optional<Answer> zones(std::string_view query, const Context& ctx) {
    const std::string q = lower(trim(query));
    auto answer = [&](local_time<minutes> t, const time_zone* z, const std::string& typed, const std::string& input,
                      const std::string& input_badge) -> std::optional<Answer> {
        Answer a;
        a.input = input;
        a.input_badge = input_badge;
        const local_days d = floor<days>(t);
        const int mins = int((t - d).count());
        a.result = clock_text(mins);
        const local_days here = today(ctx);
        if (d != here)
            a.result += (d > here ? ", tomorrow" : ", yesterday");
        a.result_badge = place_name(z, typed);
        a.copy = clock_text(mins);
        return a;
    };
    // "time in tokyo", "tokyo time"
    std::string place;
    if (q.starts_with("time in "))
        place = q.substr(8);
    else if (q.ends_with(" time") && q.size() > 5)
        place = q.substr(0, q.size() - 5);
    if (!place.empty()) {
        const time_zone* z = place_zone(place);
        if (!z)
            return std::nullopt;
        return answer(floor<minutes>(z->to_local(ctx.now)), z, place, trim(query), "Now");
    }
    // "5pm in tokyo" (from here), "5pm london in tokyo", "5pm london to sf"
    std::vector<std::string> w = words(q);
    size_t k = 0;
    auto c = clock_of(w, k);
    if (!c)
        return std::nullopt;
    size_t sep = std::string::npos;
    for (size_t j = k; j < w.size(); ++j)
        if (w[j] == "in" || w[j] == "to")
            sep = j;
    if (sep == std::string::npos || sep + 1 >= w.size())
        return std::nullopt;
    auto join = [&](size_t from, size_t to) {
        std::string s;
        for (size_t j = from; j < to; ++j)
            s += (s.empty() ? "" : " ") + w[j];
        return s;
    };
    const std::string from_place = join(k, sep), to_place = join(sep + 1, w.size());
    const time_zone* from = from_place.empty() ? zone_of(ctx) : place_zone(from_place);
    const time_zone* to = place_zone(to_place);
    if (!from || !to)
        return std::nullopt;
    const local_time<minutes> there_today = floor<days>(floor<minutes>(from->to_local(ctx.now))) + minutes(c->minutes);
    sys_time<minutes> at;
    try {
        at = floor<minutes>(from->to_sys(there_today, choose::earliest));
    } catch (...) {
        return std::nullopt;
    }
    const auto result = floor<minutes>(to->to_local(at));
    Answer a = *answer(result, to, to_place, trim(query), from_place.empty() ? "Here" : place_name(from, from_place));
    // Relative to the day it started on, not to today here.
    const local_days start = floor<days>(there_today), end_day = floor<days>(result);
    a.result = clock_text(int((result - end_day).count())) +
               (end_day > start ? ", next day" : end_day < start ? ", day before" : "");
    return a;
}

} // namespace

std::optional<Answer> evaluate(std::string_view query, const Context& ctx) {
    const std::string q = trim(query);
    if (q.empty() || q.size() > 200)
        return std::nullopt;
    try {
        if (auto a = zones(q, ctx))
            return a;
    } catch (...) {
    }
    try {
        if (auto a = dates(q, ctx))
            return a;
    } catch (...) {
    }
    try {
        return expression(q, ctx);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Rates> parse_ecb(std::string_view xml) {
    Rates r;
    size_t at = 0;
    if (const size_t t = xml.find("time='"); t != std::string_view::npos)
        r.date = std::string(xml.substr(t + 6, 10));
    while ((at = xml.find("currency='", at)) != std::string_view::npos) {
        const std::string code(xml.substr(at + 10, 3));
        const size_t rate = xml.find("rate='", at);
        if (rate == std::string_view::npos)
            break;
        const double v = std::strtod(std::string(xml.substr(rate + 6, 20)).c_str(), nullptr);
        if (v > 0)
            r.per_euro[code] = v;
        at = rate;
    }
    if (r.per_euro.empty())
        return std::nullopt;
    return r;
}

} // namespace atrium::calc
