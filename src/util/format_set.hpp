#pragma once
// DRM formats, each with the modifiers it comes in: what a plane scans out,
// a renderer draws to or samples from, a client is told it can send.
#include <cstdint>
#include <vector>

namespace atrium {

struct DrmFormat {
    uint32_t format = 0;  // DRM_FORMAT_*
    std::vector<uint64_t> modifiers;
    bool has(uint64_t modifier) const;
};

class FormatSet {
public:
    // False if it was already in.
    bool add(uint32_t format, uint64_t modifier);
    const DrmFormat* get(uint32_t format) const;
    bool has(uint32_t format, uint64_t modifier) const;
    void clear() { formats_.clear(); }
    bool empty() const { return formats_.empty(); }
    size_t size() const { return formats_.size(); }
    auto begin() const { return formats_.begin(); }
    auto end() const { return formats_.end(); }

private:
    std::vector<DrmFormat> formats_;  // in the order they came
};

} // namespace atrium
