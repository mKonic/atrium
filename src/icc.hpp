#pragma once
// A display's colour profile (ICC) as a 3D table the output pass looks up.

#include <cstdint>
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

// A display's calibration for its HDR mode: an ICC profile with Microsoft's
// MHC2 tag, as Windows HDR Calibration and DisplayCAL write them. Its
// matrix (on XYZ) as a change to linear BT.2020, applied before PQ; its
// per-channel curves on the PQ signal as a table like Lut's (size 0 when it
// has none); and the luminances it measured (0 when it doesn't say).
struct HdrCalibration {
    float matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    Lut lut;
    double min_nits = 0, max_nits = 0;
    std::string description;
};

std::optional<HdrCalibration> load_hdr(const std::string& path, std::string* error, int size = 65);

// The MHC2 tag itself (learn.microsoft.com, "Display calibration (MHC)"):
// big-endian, offsets from the tag's start. Exposed for tests.
struct Mhc2 {
    double min_nits = 0, max_nits = 0;
    float xyz[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major; the tag's offset column ignored, as Windows does
    std::vector<float> red, green, blue;           // regamma: the PQ signal in, out
};
std::optional<Mhc2> parse_mhc2(const std::vector<uint8_t>& tag);

} // namespace atrium::icc
