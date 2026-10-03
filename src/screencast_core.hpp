#pragma once
// Screen casting's pure parts: formats between DRM and PipeWire, the
// modifier a stream's buffers get, and frame pacing.
#include <cstdint>
#include <optional>
#include <vector>

namespace atrium {

// DRM fourcc <-> spa_video_format (0 / DRM_FORMAT_INVALID when unknown).
uint32_t screencast_spa_format(uint32_t drm_format);
uint32_t screencast_drm_format(uint32_t spa_format);

// The modifiers to try, in the consumer's order of preference, of those both
// sides can use. The implicit one (DRM_FORMAT_MOD_INVALID) only when it is
// all that is left: it means "whatever the driver likes", which another
// process may read wrong.
std::vector<uint64_t> screencast_modifiers(const std::vector<uint64_t>& offered,
                                           const std::vector<uint64_t>& supported);

// How long until the next frame may go at `num`/`den` frames a second at
// most (0: now). No limit when `num` is 0.
int64_t screencast_wait_ns(int64_t last_ns, int64_t now_ns, uint32_t num, uint32_t den);

} // namespace atrium
