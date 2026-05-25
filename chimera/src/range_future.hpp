#pragma once

#include <chrono>
#include <cstdint>
#include <vector>
#include <stdexcept>

#include "chimera_state.hpp"
#include "register.hpp"

namespace chimera {

class RangeFuture : public BasicFuture {
public:
    enum Step {
        Reading,
        Done
    };

private:
    Step step = Done;
    uint64_t start_key = 0;
    uint64_t range_len = 0;

    Register* bulk_bufs;   // base; per-server slice = bulk_bufs + r * max_range

    std::vector<int64_t> ongoing_per_server;
    std::vector<uint64_t> results_;

    bool measuring = false;
    timepoint start_time;

public:
    RangeFuture(ChimeraState& s, uint64_t id) : BasicFuture{s, id} {
        ongoing_per_server.assign(state.layout.num_servers, 0);
        bulk_bufs = state.layout.getBulkBufs(future_id);
        results_.reserve(state.layout.max_range);
    }

    void doRange(uint64_t sk, uint64_t ek, bool _measuring = false) {
        start_key = sk;
        range_len = (ek - sk) + 1;
        measuring = _measuring;
        if (measuring) start_time = std::chrono::steady_clock::now();

        if (range_len > state.layout.max_range) {
            throw std::runtime_error("Range exceeds max_range");
        }

        // Issue one bulk read per quorum replica.
        for (size_t i = 0; i < state.quorum; ++i) {
            size_t r = state.quorum_indices[i];
            auto& rc = *state.server_conns[r];

            Register* dst = bulk_bufs + r * state.layout.max_range;
            size_t bytes = range_len * sizeof(Register);

            rc.postSendSingle(
                dory::conn::ReliableConnection::RdmaRead,
                future_id,
                dst,
                bytes,
                Layout::remoteAddrOf(rc.remoteBuf(), start_key));

            ongoing_per_server[r] += 1;
            state.to_poll_per_server[r] += 1;
        }
        step = Reading;
    }

    bool isDone() const { return step == Done; }

    std::vector<uint64_t> const& getResults() const { return results_; }

    timepoint getStart() const { return start_time; }
    bool isMeasuring() const { return measuring; }

    void addToOngoingRDMA(size_t server_idx, int64_t n) {
        ongoing_per_server[server_idx] += n;
    }

    bool tryStepForward() {
        for (auto x : ongoing_per_server) if (x > 0) return false;

        switch (step) {
            case Reading: {
                results_.clear();
                results_.resize(range_len);

                // For each key offset in the range, take the max register
                // across the quorum, then extract its value.
                for (size_t off = 0; off < range_len; ++off) {
                    Register max_reg(0);
                    for (size_t i = 0; i < state.quorum; ++i) {
                        size_t r = state.quorum_indices[i];
                        Register reg = bulk_bufs[r * state.layout.max_range + off];
                        state.countRead();
                        if (max_reg < reg) max_reg = reg;
                    }
                    results_[off] = max_reg.fields.value;
                }
                if (measuring) {
                    state.addRangeMeasurement(start_time, std::chrono::steady_clock::now());
                }
                step = Done;
                break;
            }
            case Done:
                break;
        }
        return true;
    }
};

} // namespace chimera
