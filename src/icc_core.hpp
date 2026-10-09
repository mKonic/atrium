#pragma once
// A display's colour profile (ICC), turned into what the renderer applies to
// the finished frame, the way KWin applies one (iccprofile.cpp,
// icc_shader.cpp, drm_output.cpp):
//
// - A plain profile, on an SDR screen: what atrium draws (sRGB, gamma 2.2)
//   carried into the screen's own colours through the profile (perceptual
//   intent, so its BToA0 tag when it has one), then its calibration curves
//   (vcgt). All of it baked into one 3D table on the encoded signal.
// - A profile with an MHC2 tag (Windows HDR Calibration, DisplayCAL): its
//   matrix in linear light, then the screen's transfer function (gamma 2.2,
//   or PQ in HDR), then the tag's curves on that signal. An HDR calibration
//   must have one; its peak becomes the screen's.
#include <array>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

struct ColorCorrection {
    // On the encoded signal: lut_size^3 RGB triples, red varying fastest,
    // then green, then blue. Empty: none.
    int lut_size = 0;
    std::vector<float> lut;
    // In linear light, after everything else the renderer does there (row
    // major). None: identity.
    std::optional<std::array<float, 9>> calibration;
    // The screen's peak in nits (the MHC2 tag's), for HDR.
    std::optional<double> peak_nits;
};

constexpr int kIccLutSize = 33;

std::expected<ColorCorrection, std::string> icc_for_sdr(const std::vector<unsigned char>& profile);
std::expected<ColorCorrection, std::string> icc_for_hdr(const std::vector<unsigned char>& profile);
// The file's bytes, or why not.
std::expected<std::vector<unsigned char>, std::string> read_icc(const std::string& path);

} // namespace atrium
