#pragma once

#include <chrono>
#include <cstdint>
#include <vector>
#include <algorithm>

#include <dory/extern/ibverbs.hpp>
#include "chimera_state.hpp"
#include "register.hpp"

namespace chimera {

enum PutPerfMode {
    RAW_WRITE,     // Mode 1: Just fire an RDMA Write to all servers (No read, no atomics, zero tracking)
    BLIND_CAS,     // Mode 2: Fire a single CAS to all servers based on a blind guess (No retry if it fails)
    FULL_PROTOCOL  // Mode 3: Your updated, single-CAS optimistic correctness protocol
};

class PutFuturePerf : public BasicFuture {
public:
    enum Step {
        OptimisticCAS,
        CAS,
        Done
    };

private:
    Step step = Done;
    PutPerfMode mode;
    
    uint64_t key;
    uint64_t value;

    Register* read_bufs;
    Register* swap_bufs;

    std::vector<int64_t> ongoing_per_server;
    std::vector<bool> needs_cas;

    uint64_t expected[8];
    Register target_reg;
    uint64_t success_count = 0;

    bool measuring = false;
    timepoint start;

    void postRawWrite() {
        uint64_t wr_id = this->future_id; 

        // Fix: Explicitly qualify std::cout and std::endl
        // std::cout << "Posted RAW WRITE for key " << key << " with value " << value << " to all servers." << std::endl;

        for (size_t i = 0; i < state.quorum; ++i) {
            size_t r = state.quorum_indices[i];
            auto& rc = *state.server_conns[r];
            swap_bufs[r] = target_reg; 

            rc.postSendSingle(
                dory::conn::ReliableConnection::RdmaWrite,
                wr_id, 
                &swap_bufs[r], 
                sizeof(Register),
                Layout::remoteAddrOf(rc.remoteBuf(), key));

            ongoing_per_server[r] += 1;
            state.to_poll_per_server[r] += 1;
        }
    }

    // Single CAS execution that leaves the ongoing state tracked but ignored later
    void postBlindCas() {
        for (size_t i = 0; i < state.quorum; ++i) {
            size_t r = state.quorum_indices[i];
            auto& rc = *state.server_conns[r];

            rc.postSendSingleCas(
                future_id,
                &swap_bufs[r],
                Layout::remoteAddrOf(rc.remoteBuf(), key),
                expected[r],
                target_reg.raw);

            ongoing_per_server[r] += 1;
            state.to_poll_per_server[r] += 1;
            state.countCasAttempt();
        }
    }

    void postCas() {
        for (size_t i = 0; i < state.quorum; ++i) {
            size_t r = state.quorum_indices[i];
            if (!needs_cas[r]) continue;
            auto& rc = *state.server_conns[r];

            rc.postSendSingleCas(
                future_id,
                &swap_bufs[r],
                Layout::remoteAddrOf(rc.remoteBuf(), key),
                expected[i],
                target_reg.raw);

            ongoing_per_server[r] += 1;
            state.to_poll_per_server[r] += 1;
            state.countCasAttempt();
        }
    }

public:
    PutFuturePerf(ChimeraState& s, uint64_t id) : BasicFuture{s, id} {
        ongoing_per_server.assign(state.layout.num_servers, 0);
        needs_cas.assign(state.layout.num_servers, false);
        read_bufs = state.layout.getReadBufs(future_id);
        swap_bufs = state.layout.getSwapBufs(future_id);
    }

    // Extended execution variant accepting the dynamic mode profile
    void doPutPerf(uint64_t k, uint64_t v, PutPerfMode testing_mode, bool _measuring = false) {
        key = k;
        value = v;
        mode = testing_mode;
        measuring = _measuring;
        if (measuring) start = std::chrono::steady_clock::now();

        success_count = 0;
        std::fill(needs_cas.begin(), needs_cas.end(), false);

        uint16_t simulated_seq = 100; 
        target_reg = Register(simulated_seq, static_cast<uint16_t>(state.client_idx), static_cast<uint32_t>(value));

        if (mode == RAW_WRITE) {
            // Set an active state BEFORE checking if we are measuring
            step = OptimisticCAS; // We can reuse OptimisticCAS as our "Active/Working" token
            postRawWrite();
            
            if (!measuring) {
                bool drained = false;
                while (!drained) {
                    drained = true;
                    for (auto x : ongoing_per_server) if (x > 0) drained = false;
                }
                step = Done;
            }
            return; 
        }

        // Setup baseline guesses for atomic tests
        for (size_t i = 0; i < state.layout.num_servers; ++i) {
            expected[i] = 0; 
        }

        if (mode == BLIND_CAS) {
            step = OptimisticCAS; // ✓ FIXED: Mark as active so tryStepForward handles it safely
            postBlindCas();
            return;
        }
        
        // If FULL_PROTOCOL is used, initialize its starting state
        step = OptimisticCAS;
    }

    bool isDone() const { return step == Done; }
    timepoint getStart() const { return start; }
    bool isMeasuring() const { return measuring; }

    void addToOngoingRDMA(size_t server_idx, int64_t n) {
        ongoing_per_server[server_idx] += n;
    }

    bool tryStepForward() {
        // 1. Strict hardware brake: If packets are in-flight, exit immediately.
        for (auto x : ongoing_per_server) if (x > 0) return false;

        // 2. High-performance raw paths evaluation
        if (mode == RAW_WRITE || mode == BLIND_CAS) {
            // Only proceed if this slot is actively processing an operation
            if (step != Done) {
                if (this->measuring) {
                    state.addPutMeasurement(start, std::chrono::steady_clock::now());
                }
                step = Done; // Safely close the slot now
                return true; // Operation completed successfully
            }
            return true; // Already Done, safe to return
        }
        
        // 3. Full Consensus Protocol State Machine Fallback
        switch (step) {
            case OptimisticCAS: {
                uint16_t z_max = target_reg.fields.seq - 1;
                for (size_t i = 0; i < state.quorum; ++i) {
                    size_t r = state.quorum_indices[i];
                    Register observed = swap_bufs[r];

                    if (observed.raw == expected[r]) {
                        needs_cas[r] = false;
                        success_count++;
                    } else {
                        state.countCasFail();
                        expected[i] = observed.raw;
                        z_max = std::max(z_max, observed.fields.seq);
                    }
                }

                if (success_count >= state.quorum) {
#if CHIMERA_CACHE_ENABLED
                    state.cache.put(key, target_reg);
#endif
                    if (this->measuring) state.addPutMeasurement(start, std::chrono::steady_clock::now());
                    step = Done;
                    break;
                }

                state.put_retries++;
                target_reg = Register(static_cast<uint16_t>(z_max + 1), static_cast<uint16_t>(state.client_idx), static_cast<uint32_t>(value));
                postCas();
                step = CAS;
                break;
            }

            case CAS: {
                for (size_t i = 0; i < state.quorum; ++i) {
                    size_t r = state.quorum_indices[i];
                    if (!needs_cas[r]) continue;

                    Register observed = swap_bufs[r];
                    if (observed.raw == expected[i]) {
                        needs_cas[r] = false;
                        success_count++;
                    } else {
                        state.countCasFail();
                        expected[i] = observed.raw;
                        if (observed.fields.seq >= target_reg.fields.seq) {
                            target_reg = Register(static_cast<uint16_t>(observed.fields.seq + 1), static_cast<uint16_t>(state.client_idx), static_cast<uint32_t>(value));
                            for (size_t j = 0; j < state.quorum; ++j) {
                                size_t rj = state.quorum_indices[j];
                                needs_cas[rj] = true;
                                expected[j] = swap_bufs[rj].raw;
                            }
                            success_count = 0;
                        }
                    }
                }

                if (success_count >= state.quorum) {
#if CHIMERA_CACHE_ENABLED
                    state.cache.put(key, target_reg);
#endif
                    if (this->measuring) state.addPutMeasurement(start, std::chrono::steady_clock::now());
                    step = Done;
                } else {
                    state.put_retries++;
                    postCas();
                }
                break;
            }
            case Done: break;
        }
        return true;
    }
};

} // namespace chimera