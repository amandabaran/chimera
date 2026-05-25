#pragma once

#include <cstdint>
#include <vector>

#include <dory/conn/rc-exchanger.hpp>
#include <dory/extern/ibverbs.hpp>

#include "chimera_state.hpp"
#include "op_future.hpp"

namespace chimera {

class ChimeraClient {
private:
    ChimeraState state;
    std::vector<OpFuture> futures;
    std::vector<bool> progress;

public:
    ChimeraClient(Layout layout,
              dory::conn::RcConnectionExchanger<ProcId>& rcx,
              ProcId proc_id)
    : state{layout, rcx, proc_id} {
        progress.resize(state.layout.async_parallelism, false);
        futures.reserve(state.layout.async_parallelism);
        for (size_t i = 0; i < state.layout.async_parallelism; ++i) {
            futures.emplace_back(state, i);
        }
    }

    // Drain CQs across all server connections.  Routes each completion to
    // its owning future via wr_id, and flips `progress[i]` so the user loop
    // knows to call tryStepForward on it.
    bool tickRdma() {
        bool any_progress = false;
        for (size_t s = 0; s < state.layout.num_servers; ++s) {
            auto& rc = *state.server_conns[s];
            auto& tp = state.to_poll_per_server[s];
            if (tp == 0) continue;

            state.wces.resize(static_cast<size_t>(tp));
            if (!rc.pollCqIsOk(dory::conn::ReliableConnection::SendCq, state.wces)) {
                throw std::runtime_error("Error polling CQ");
            }
            for (auto const& wc : state.wces) {
                if (wc.status != IBV_WC_SUCCESS) {
                    throw std::runtime_error("WC unsuccessful");
                }
                futures.at(wc.wr_id).addToOngoingRDMA(s, -1);
                progress.at(wc.wr_id) = true;
                any_progress = true;
            }
            tp -= static_cast<int64_t>(state.wces.size());
        }
        return any_progress;
    }

    // Returns the first future that is done.  Spins on tickRdma() until one is.
    OpFuture& getFreeFuture() {
        while (true) {
            for (uint64_t i = 0; i < state.layout.async_parallelism; ++i) {
                auto& f = futures[i];
                if (progress[i]) {
                    f.tryStepForward();
                    progress[i] = false;
                }
                if (f.isDone()) return f;
            }
            tickRdma();
        }
    }

    // Returns a specific future by index (used for round-robin or hashing).
    OpFuture& getFuture(uint64_t i) {
        return futures.at(i % state.layout.async_parallelism);
    }

    // Wait for all futures to drain — call this at end of measurement.
    void finishAllFutures() {
        for (auto& f : futures) {
            while (!f.isDone()) {
                tickRdma();
                for (uint64_t i = 0; i < state.layout.async_parallelism; ++i) {
                    if (progress[i]) {
                        futures[i].tryStepForward();
                        progress[i] = false;
                    }
                }
            }
        }
    }

    ChimeraState& getState() { return state; }

    void reportStats(bool detailed = false) {
        state.reportStats(detailed);
    }
};

} // namespace chimera
