#include "accent.hpp"

#include <algorithm>
#include <cmath>

namespace atrium::accent {

namespace {

struct Entry {
    std::string_view name, label;
    uint32_t rgb, light;
    std::string_view gnome;
};

// macOS's accent colours, as drawn in dark and in light mode, and GNOME's nearest.
constexpr Entry kColors[] = {
    {"blue", "Blue", 0x0a84ff, 0x007aff, "blue"},         {"purple", "Purple", 0xbf5af2, 0xaf52de, "purple"},
    {"pink", "Pink", 0xff375f, 0xff2d55, "pink"},         {"red", "Red", 0xff453a, 0xff3b30, "red"},
    {"orange", "Orange", 0xff9f0a, 0xff9500, "orange"},   {"yellow", "Yellow", 0xffd60a, 0xffcc00, "yellow"},
    {"green", "Green", 0x32d74b, 0x28cd41, "green"},      {"graphite", "Graphite", 0x98989d, 0x8e8e93, "slate"},
};

struct Lab {
    double l, a, b;
};

double to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double from_linear(double c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

// D65 white.
constexpr double kXn = 0.95047, kYn = 1.0, kZn = 1.08883;

double f(double t) {
    return t > 216.0 / 24389 ? std::cbrt(t) : (24389.0 / 27 * t + 16) / 116;
}

double f_inv(double t) {
    return t * t * t > 216.0 / 24389 ? t * t * t : (116 * t - 16) / (24389.0 / 27);
}

Lab to_lab(uint32_t rgb) {
    const double r = to_linear(((rgb >> 16) & 0xff) / 255.0);
    const double g = to_linear(((rgb >> 8) & 0xff) / 255.0);
    const double b = to_linear((rgb & 0xff) / 255.0);
    const double x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
    const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    const double z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
    const double fx = f(x / kXn), fy = f(y / kYn), fz = f(z / kZn);
    return {116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
}

// Linear sRGB of a Lab colour; may fall outside 0..1.
void to_linear_rgb(const Lab& c, double out[3]) {
    const double fy = (c.l + 16) / 116, fx = fy + c.a / 500, fz = fy - c.b / 200;
    const double x = kXn * f_inv(fx), y = kYn * f_inv(fy), z = kZn * f_inv(fz);
    out[0] = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    out[1] = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    out[2] = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
}

bool in_gamut(const Lab& c) {
    double rgb[3];
    to_linear_rgb(c, rgb);
    return std::ranges::all_of(rgb, [](double v) { return v >= -1e-4 && v <= 1 + 1e-4; });
}

uint32_t pack(const Lab& c) {
    double rgb[3];
    to_linear_rgb(c, rgb);
    uint32_t out = 0;
    for (double v : rgb)
        out = out << 8 | uint32_t(std::lround(std::clamp(from_linear(std::clamp(v, 0.0, 1.0)), 0.0, 1.0) * 255));
    return out;
}

} // namespace

const std::vector<std::string_view>& names() {
    static const std::vector<std::string_view> all = [] {
        std::vector<std::string_view> v{"multicolor"};
        for (const Entry& e : kColors)
            v.push_back(e.name);
        return v;
    }();
    return all;
}

std::optional<uint32_t> seed(std::string_view name) {
    for (const Entry& e : kColors)
        if (e.name == name)
            return e.rgb;
    return std::nullopt;
}

uint32_t rgb(std::string_view name, bool light) {
    for (const Entry& e : kColors)
        if (e.name == name)
            return light ? e.light : e.rgb;
    return light ? 0x007aff : 0x0a84ff;  // multicolour: the system blue
}

std::string_view label(std::string_view name) {
    for (const Entry& e : kColors)
        if (e.name == name)
            return e.label;
    return "Multicolour";
}

std::string_view gnome_name(std::string_view name) {
    for (const Entry& e : kColors)
        if (e.name == name)
            return e.gnome;
    return "blue";
}

double lightness(uint32_t rgb) {
    return to_lab(rgb).l;
}

uint32_t tone(uint32_t rgb, double l, double max_chroma) {
    const Lab seed = to_lab(rgb);
    const double hue = std::atan2(seed.b, seed.a);
    // The most chroma sRGB holds at this lightness and hue: bisect.
    double lo = 0, hi = std::min(std::hypot(seed.a, seed.b), max_chroma);
    auto at = [&](double c) { return Lab{l, c * std::cos(hue), c * std::sin(hue)}; };
    if (!in_gamut(at(hi)))
        for (int i = 0; i < 30; ++i) {
            const double mid = (lo + hi) / 2;
            (in_gamut(at(mid)) ? lo : hi) = mid;
        }
    else
        lo = hi;
    return pack(at(lo));
}

} // namespace atrium::accent
