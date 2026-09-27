#pragma once
// A display's colour profile (ICC) as a 3D table the output pass looks up.

#include <optional>
#include <string>
#include <vector>

namespace atrium::icc {

// From the frame's signal (sRGB primaries, gamma 2.2, as atrium draws SDR)
// to the display's own, relative colorimetric: `size`³ RGB triples, red
// fastest, then green, then blue (a GL 3D texture's x, y, z).
struct Lut {
    int size = 0;
    std::vector<float> rgb;
    std::string description;  // the profile's own name for itself
};

std::optional<Lut> load(const std::string& path, std::string* error, int size = 33);

} // namespace atrium::icc
