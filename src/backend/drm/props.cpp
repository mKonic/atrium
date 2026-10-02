#include "backend/drm/props.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstring>
#include <utility>

namespace atrium::backend::drm {

namespace {

struct Named {
    const char* name;
    uint32_t* id;
};

bool scan(int fd, uint32_t id, uint32_t type, std::initializer_list<Named> want) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, id, type);
    if (!props)
        return false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        for (const Named& n : want)
            if (std::strcmp(p->name, n.name) == 0)
                *n.id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return true;
}

} // namespace

bool get_props(int fd, uint32_t id, ConnectorProps* o) {
    return scan(fd, id, DRM_MODE_OBJECT_CONNECTOR,
                {{"CRTC_ID", &o->crtc_id},
                 {"EDID", &o->edid},
                 {"DPMS", &o->dpms},
                 {"link-status", &o->link_status},
                 {"non-desktop", &o->non_desktop},
                 {"content type", &o->content_type},
                 {"max bpc", &o->max_bpc},
                 {"Colorspace", &o->colorspace},
                 {"HDR_OUTPUT_METADATA", &o->hdr_output_metadata},
                 {"vrr_capable", &o->vrr_capable},
                 {"subconnector", &o->subconnector},
                 {"panel orientation", &o->panel_orientation}});
}

bool get_props(int fd, uint32_t id, CrtcProps* o) {
    return scan(fd, id, DRM_MODE_OBJECT_CRTC,
                {{"ACTIVE", &o->active},
                 {"MODE_ID", &o->mode_id},
                 {"GAMMA_LUT", &o->gamma_lut},
                 {"GAMMA_LUT_SIZE", &o->gamma_lut_size},
                 {"VRR_ENABLED", &o->vrr_enabled},
                 {"OUT_FENCE_PTR", &o->out_fence_ptr}});
}

bool get_props(int fd, uint32_t id, PlaneProps* o) {
    return scan(fd, id, DRM_MODE_OBJECT_PLANE,
                {{"type", &o->type},
                 {"FB_ID", &o->fb_id},
                 {"CRTC_ID", &o->crtc_id},
                 {"SRC_X", &o->src_x},
                 {"SRC_Y", &o->src_y},
                 {"SRC_W", &o->src_w},
                 {"SRC_H", &o->src_h},
                 {"CRTC_X", &o->crtc_x},
                 {"CRTC_Y", &o->crtc_y},
                 {"CRTC_W", &o->crtc_w},
                 {"CRTC_H", &o->crtc_h},
                 {"IN_FORMATS", &o->in_formats},
                 {"IN_FENCE_FD", &o->in_fence_fd},
                 {"FB_DAMAGE_CLIPS", &o->fb_damage_clips},
                 {"HOTSPOT_X", &o->hotspot_x},
                 {"HOTSPOT_Y", &o->hotspot_y},
                 {"SIZE_HINTS", &o->size_hints}});
}

bool get_prop(int fd, uint32_t object, uint32_t prop, uint64_t* value) {
    if (!prop)
        return false;
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, object, DRM_MODE_OBJECT_ANY);
    if (!props)
        return false;
    bool found = false;
    for (uint32_t i = 0; i < props->count_props; ++i)
        if (props->props[i] == prop) {
            *value = props->prop_values[i];
            found = true;
            break;
        }
    drmModeFreeObjectProperties(props);
    return found;
}

std::vector<uint8_t> get_prop_blob(int fd, uint32_t object, uint32_t prop) {
    uint64_t blob_id = 0;
    if (!get_prop(fd, object, prop, &blob_id) || !blob_id)
        return {};
    drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd, uint32_t(blob_id));
    if (!blob)
        return {};
    std::vector<uint8_t> out(static_cast<const uint8_t*>(blob->data),
                             static_cast<const uint8_t*>(blob->data) + blob->length);
    drmModeFreePropertyBlob(blob);
    return out;
}

std::string get_prop_enum(int fd, uint32_t object, uint32_t prop) {
    uint64_t value;
    if (!get_prop(fd, object, prop, &value))
        return {};
    drmModePropertyRes* p = drmModeGetProperty(fd, prop);
    if (!p)
        return {};
    std::string out;
    for (int i = 0; i < p->count_enums; ++i)
        if (p->enums[i].value == value)
            out = p->enums[i].name;
    drmModeFreeProperty(p);
    return out;
}

bool prop_range(int fd, uint32_t prop, uint64_t* min, uint64_t* max) {
    drmModePropertyRes* p = drmModeGetProperty(fd, prop);
    if (!p)
        return false;
    const bool ok = drm_property_type_is(p, DRM_MODE_PROP_RANGE) && p->count_values == 2;
    if (ok) {
        *min = p->values[0];
        *max = p->values[1];
    }
    drmModeFreeProperty(p);
    return ok;
}

} // namespace atrium::backend::drm
