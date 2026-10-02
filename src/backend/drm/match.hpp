#pragma once
// Which CRTC drives which connector: each connector names the CRTCs it can
// use (a bit each), and as many connectors as possible get one, moving as
// few as possible from where they were. After wlroots' backend/drm/util.c
// (MIT), a small maximum bipartite matching.
#include <cstdint>
#include <vector>

namespace atrium::backend::drm {

constexpr uint32_t kUnmatched = UINT32_MAX;

// `possible[c]`: connector c's CRTCs (0: it wants none). `previous[k]`: the
// connector CRTC k drove (kUnmatched for none). Returns the same for the
// new assignment.
std::vector<uint32_t> match_crtcs(const std::vector<uint32_t>& possible, const std::vector<uint32_t>& previous);

} // namespace atrium::backend::drm
