#pragma once
// KMS object properties by name: their ids looked up once, values and blobs
// read when needed. After wlroots' backend/drm/properties.c (MIT).
#include <cstdint>
#include <string>
#include <vector>

namespace atrium::backend::drm {

struct ConnectorProps {
    uint32_t crtc_id = 0, edid = 0, dpms = 0, link_status = 0, non_desktop = 0, content_type = 0, max_bpc = 0,
             colorspace = 0, hdr_output_metadata = 0, vrr_capable = 0, subconnector = 0, panel_orientation = 0;
};
struct CrtcProps {
    uint32_t active = 0, mode_id = 0, gamma_lut = 0, gamma_lut_size = 0, vrr_enabled = 0, out_fence_ptr = 0;
};
struct PlaneProps {
    uint32_t type = 0, fb_id = 0, crtc_id = 0, src_x = 0, src_y = 0, src_w = 0, src_h = 0, crtc_x = 0, crtc_y = 0,
             crtc_w = 0, crtc_h = 0, in_formats = 0, in_fence_fd = 0, fb_damage_clips = 0, hotspot_x = 0,
             hotspot_y = 0, size_hints = 0;
};

bool get_props(int fd, uint32_t id, ConnectorProps* out);
bool get_props(int fd, uint32_t id, CrtcProps* out);
bool get_props(int fd, uint32_t id, PlaneProps* out);

bool get_prop(int fd, uint32_t object, uint32_t prop, uint64_t* value);
// A blob property's bytes (empty if unset).
std::vector<uint8_t> get_prop_blob(int fd, uint32_t object, uint32_t prop);
// An enum property's current value's name.
std::string get_prop_enum(int fd, uint32_t object, uint32_t prop);
bool prop_range(int fd, uint32_t prop, uint64_t* min, uint64_t* max);

} // namespace atrium::backend::drm
