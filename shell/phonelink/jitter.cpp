#include "jitter.hpp"

#include <algorithm>
#include <cstring>

namespace atrium::phonelink {

JitterBuffer::JitterBuffer(std::uint32_t target) : target_(target) {}

std::int64_t JitterBuffer::unwrapTs(std::uint32_t ts) {
    const std::int64_t v = lastTs_ + std::int32_t(ts - lastTs32_);
    return v;
}

std::int64_t JitterBuffer::unwrapSeq(std::uint32_t seq) {
    return haveSeq_ ? lastSeq_ + std::int32_t(seq - lastSeq32_) : 0;
}

void JitterBuffer::resync(std::int64_t ts) {
    packets_.clear();
    play_ = ts - std::int64_t(target_);
    newestEnd_ = ts;
    start_ = ts;
    // Nothing from before the gap is repeated into the new stream.
    concealGain_ = 0;
    synced_ = true;
    stats_.resyncs++;
}

void JitterBuffer::push(const AudioPacket& p, std::uint64_t now) {
    const std::uint32_t frames = p.frames();
    if (!frames)
        return;
    stats_.received++;

    // Sequence numbers: what went missing.
    const std::int64_t seq = unwrapSeq(p.seq);
    bool resent = false;
    if (!haveSeq_) {
        haveSeq_ = true;
        highestSeq_ = seq;
    } else if (seq > highestSeq_) {
        // Past a burst too long to recover, don't ask for all of it.
        for (std::int64_t s = std::max(highestSeq_ + 1, seq - 64); s < seq; s++)
            missing_[s] = {now, 0, 0};
        highestSeq_ = seq;
    } else {
        resent = missing_.erase(seq) > 0;
    }
    lastSeq_ = seq;
    lastSeq32_ = p.seq;

    // Timestamps: where it plays.
    const std::int64_t ts = synced_ ? unwrapTs(p.timestamp) : 0;
    if (!synced_) {
        lastTs_ = 0;
        lastTs32_ = p.timestamp;
        resync(0);
    } else if ((p.flags & Discontinuity) && newestEnd_ <= play_) {
        // The phone was silent and the buffer ran dry: start over at it.
        resync(ts);
    } else if (ts + frames <= play_) {
        stats_.late++;
        return;
    }
    if (ts >= lastTs_) {
        lastTs_ = ts;
        lastTs32_ = p.timestamp;
    }
    if (packets_.contains(ts)) {
        stats_.duplicates++;
        return;
    }
    if (resent)
        stats_.recovered++;
    std::vector<std::int16_t> pcm(frames * kChannels);
    std::memcpy(pcm.data(), p.pcm.data(), pcm.size() * 2);
    packets_.emplace(ts, std::move(pcm));
    newestEnd_ = std::max(newestEnd_, ts + std::int64_t(frames));

    // Far behind (a stall upstream let a pile build): jump to the target.
    if (newestEnd_ - play_ > std::int64_t(target_) * 8) {
        const std::int64_t to = newestEnd_ - target_;
        stats_.skipped += std::uint64_t(to - play_);
        play_ = to;
        while (!packets_.empty() && packets_.begin()->first + std::int64_t(packets_.begin()->second.size() / kChannels) <= play_)
            packets_.erase(packets_.begin());
    }
}

void JitterBuffer::conceal(std::int16_t* out, std::uint32_t frames) {
    // The last packet played again, fading out over a packet's length, then
    // silence.
    const std::uint32_t lastFrames = std::uint32_t(last_.size() / kChannels);
    for (std::uint32_t i = 0; i < frames; i++) {
        if (!lastFrames || concealGain_ <= 0) {
            out[i * kChannels] = out[i * kChannels + 1] = 0;
            continue;
        }
        const std::uint32_t at = concealPos_++ % lastFrames;
        for (std::uint32_t c = 0; c < kChannels; c++)
            out[i * kChannels + c] = std::int16_t(float(last_[at * kChannels + c]) * concealGain_);
        concealGain_ = std::max(0.f, concealGain_ - 1.f / float(kPacketFrames));
    }
}

void JitterBuffer::pull(std::int16_t* out, std::uint32_t frames) {
    if (!synced_) {
        std::memset(out, 0, std::size_t(frames) * kChannels * 2);
        return;
    }
    std::uint32_t done = 0;
    while (done < frames) {
        // Packets wholly behind the play position are spent.
        while (!packets_.empty() && packets_.begin()->first + std::int64_t(packets_.begin()->second.size() / kChannels) <= play_)
            packets_.erase(packets_.begin());
        const std::uint32_t want = frames - done;
        auto it = packets_.begin();
        if (it != packets_.end() && it->first <= play_) {
            const auto& pcm = it->second;
            const std::int64_t offset = play_ - it->first;
            const std::uint32_t n = std::min<std::uint32_t>(want, std::uint32_t(pcm.size() / kChannels - offset));
            std::memcpy(out + done * kChannels, pcm.data() + offset * kChannels, std::size_t(n) * kChannels * 2);
            // Remember what played, for concealing a loss right after.
            last_.assign(pcm.begin(), pcm.end());
            concealPos_ = std::uint32_t(offset + n);
            concealGain_ = 1;
            play_ += n;
            done += n;
            continue;
        }
        // Nothing here: a loss before the next packet, or past the newest.
        std::uint32_t n = want;
        if (it != packets_.end())
            n = std::uint32_t(std::min<std::int64_t>(want, it->first - play_));
        conceal(out + done * kChannels, n);
        // Concealed audio inside the stream is a loss; running past the end
        // with nothing more coming is the phone being quiet.
        if (it != packets_.end() && play_ >= start_)
            stats_.concealed += n;
        play_ += n;
        done += n;
    }
}

std::vector<std::uint32_t> JitterBuffer::nacks(std::uint64_t now) {
    std::vector<std::uint32_t> out;
    // A packet stops being worth asking for once a resend couldn't play.
    const std::uint64_t giveUp = std::uint64_t(target_) * 1'000'000 / kRate;
    for (auto it = missing_.begin(); it != missing_.end();) {
        Missing& m = it->second;
        if (m.tries >= kNackTries || now - m.since > giveUp) {
            it = missing_.erase(it);
            continue;
        }
        if (m.tries == 0 || now - m.asked >= kNackEvery) {
            m.asked = now;
            m.tries++;
            out.push_back(std::uint32_t(it->first));
            stats_.nacked++;
        }
        ++it;
    }
    return out;
}

double Drift::update(std::int64_t level, std::uint32_t target, std::uint32_t period) {
    // About a second to smooth the level over.
    const double alpha = std::min(1.0, double(period) / kRate);
    if (!primed_) {
        smooth_ = double(level);
        primed_ = true;
    }
    smooth_ += alpha * (double(level) - smooth_);
    // In seconds: positive when there is more buffered than wanted.
    const double error = (smooth_ - double(target)) / kRate;
    const double dt = double(period) / kRate;
    integral_ = std::clamp(integral_ + error * dt, -kMax / 0.01, kMax / 0.01);
    rate_ = std::clamp(1.0 + 0.1 * error + 0.01 * integral_, 1.0 - kMax, 1.0 + kMax);
    return rate_;
}

} // namespace atrium::phonelink
