#include "frame_timing.hpp"

#include <linux/sync_file.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <vector>

namespace atrium::frame_timing {

void RenderJournal::add(int64_t ns) {
    if (ns <= 0)
        return;
    // Up at once; down a twentieth of the way each frame.
    estimate_ = ns >= estimate_ ? ns : estimate_ - (estimate_ - ns) / 20;
}

int64_t margin(int64_t estimate, int64_t slack, int64_t min, int64_t period) {
    const int64_t most = period > 0 ? period / 2 : estimate + slack;
    return std::clamp(estimate + slack, std::min(min, most), most);
}

int64_t fence_signalled_ns(int fd) {
    if (fd < 0)
        return 0;
    sync_file_info info{};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.num_fences == 0)
        return 0;
    std::vector<sync_fence_info> fences(info.num_fences);
    info.sync_fence_info = reinterpret_cast<uint64_t>(fences.data());
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.status != 1)
        return 0;
    // All of them signalled: the last one is when the work was done.
    int64_t last = 0;
    for (const sync_fence_info& f : fences) {
        if (f.status != 1)
            return 0;
        last = std::max(last, int64_t(f.timestamp_ns));
    }
    return last;
}

} // namespace atrium::frame_timing
