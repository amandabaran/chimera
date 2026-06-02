#pragma once

#include <chrono>
#include <cstdint>
#include <vector>
#include <algorithm>

#include <dory/extern/ibverbs.hpp>
#include "chimera_state.hpp"
#include "register.hpp"

namespace chimera {

class PutFuture : public BasicFuture {
public:
    enum Step {
        ReadSeq,         // Phase 1 (cache miss path): read all replicas
        OptimisticCAS,   // Optimized cache-hit path: direct single CAS using cached guess
        CAS,             // Phase 2: CAS retries
        Done
    };

private:
    Step step = Done;
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

    // ─── Phase 1 (cache miss): plain READ to all quorum ──────────────
    void postReads() {
        for (size_t i = 0; i < state.quorum; ++i) {
            size_t r = state.quorum_indices[i];
            auto& rc = *state.server_conns[r];

            rc.postSendSingle(
                dory::conn::ReliableConnection::RdmaRead,
                future_id,
                &read_bufs[r],
                sizeof(Register),
                Layout::remoteAddrOf(rc.remoteBuf(), key));

            ongoing_per_server[r] += 1;
            state.to_poll_per_server[r] += 1;
        }
    }

    // ─── Optimized Path: Single Optimistic CAS ───────────────────────
    // No chained READ. If the CAS fails, the hardware returns the 
    // current remote value directly into swap_bufs[server_idx].
    void postOptimisticCas(size_t server_idx) {
        auto& rc = *state.server_conns[server_idx];
        uintptr_t remote = Layout::remoteAddrOf(rc.remoteBuf(), key);

        rc.postSendSingleCas(
            future_id,
            &swap_bufs[server_idx],
            remote,
            expected[server_idx],   // Guess from cache
            target_reg.raw);

        ongoing_per_server[server_idx] += 1;
        state.to_poll_per_server[server_idx] += 1;
        state.countCasAttempt();
    }

    // ─── Phase 2: regular CAS to laggards ────────────────────────────
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
    PutFuture(ChimeraState& s, uint64_t id) : BasicFuture{s, id} {
        ongoing_per_server.assign(state.layout.num_servers, 0);
        needs_cas.assign(state.layout.num_servers, false);
        read_bufs = state.layout.getReadBufs(future_id);
        swap_bufs = state.layout.getSwapBufs(future_id);
    }

    void doPut(uint64_t k, uint64_t v, bool _measuring = false) {
        key = k;
        value = v;
        measuring = _measuring;
        if (measuring) start = std::chrono::steady_clock::now();

        success_count = 0;
        std::fill(needs_cas.begin(), needs_cas.end(), false);

#if CHIMERA_CACHE_ENABLED
        Register cached;
        if (state.cache.get(key, cached)) {
            state.metrics_cache_hit();

            // Build target = (cached.seq + 1, client_id, value)
            target_reg = Register(
                static_cast<uint16_t>(cached.fields.seq + 1),
                static_cast<uint16_t>(state.client_idx),
                static_cast<uint32_t>(value));

            // Set expectations from cache
            for (size_t i = 0; i < state.layout.num_servers; ++i) {
                expected[i] = cached.raw;
            }
            for (size_t i = 0; i < state.quorum; ++i) {
                size_t r = state.quorum_indices[i];
                needs_cas[r] = true;
                postOptimisticCas(r);
            }
            step = OptimisticCAS;
            return;
        }
        state.metrics_cache_miss();
#endif

        postReads();
        step = ReadSeq;
    }

    bool isDone() const { return step == Done; }
    timepoint getStart() const { return start; }
    bool isMeasuring() const { return measuring; }

    void addToOngoingRDMA(size_t server_idx, int64_t n) {
        ongoing_per_server[server_idx] += n;
    }

    bool tryStepForward() {
        for (auto x : ongoing_per_server) if (x > 0) return false;

        switch (step) {
            case ReadSeq: {
                uint16_t z_max = 0;
                for (size_t i = 0; i < state.quorum; ++i) {
                    size_t r = state.quorum_indices[i];
                    Register reg = read_bufs[r];
                    expected[i] = reg.raw;
                    state.countRead();
                    z_max = std::max(z_max, reg.fields.seq);
                    needs_cas[r] = true;
                }
                target_reg = Register(
                    static_cast<uint16_t>(z_max + 1),
                    static_cast<uint16_t>(state.client_idx),
                    static_cast<uint32_t>(value));
                postCas();
                step = CAS;
                break;
            }

            // ─── Optimized Optimistic CAS Execution ──────────────────
            case OptimisticCAS: {
                uint16_t z_max = target_reg.fields.seq - 1; 
                bool any_failed = false;

                for (size_t i = 0; i < state.quorum; ++i) {
                    size_t r = state.quorum_indices[i];
                    Register observed = swap_bufs[r];

                    if (observed.raw == expected[r]) {
                        // CAS Succeeded! The remote slot matched our cached guess.
                        needs_cas[r] = false;
                        success_count++;
                    } else {
                        // CAS Failed. The HCA automatically returned the true,
                        // current remote slot data inside 'observed'.
                        state.countCasFail();
                        any_failed = true;
                        
                        expected[i] = observed.raw; // Safe for Phase 2 retries
                        z_max = std::max(z_max, observed.fields.seq);
                    }
                }

                // Normalize success tracks
                for (size_t i = 0; i < state.quorum; ++i) {
                    size_t r = state.quorum_indices[i];
                    if (!needs_cas[r]) {
                        expected[i] = target_reg.raw;
                    }
                }

                if (success_count >= state.quorum) {
#if CHIMERA_CACHE_ENABLED
                    state.cache.put(key, target_reg);
#endif
                    if (measuring) {
                        state.addPutMeasurement(start, std::chrono::steady_clock::now());
                    }
                    step = Done;
                    break;
                }

                // Fallback: Increment version sequence over largest observed state
                state.put_retries++;
                target_reg = Register(
                    static_cast<uint16_t>(z_max + 1),
                    static_cast<uint16_t>(state.client_idx),
                    static_cast<uint32_t>(value));

                postCas();
                step = CAS;
                break;
            }

            case CAS: {
                bool any_failed = false;

                for (size_t i = 0; i < state.quorum; ++i) {
                    size_t r = state.quorum_indices[i];
                    if (!needs_cas[r]) continue;

                    Register observed = swap_bufs[r];
                    if (observed.raw == expected[i]) {
                        needs_cas[r] = false;
                        success_count++;
                    } else {
                        state.countCasFail();
                        any_failed = true;
                        expected[i] = observed.raw;

                        if (observed.fields.seq >= target_reg.fields.seq) {
                            target_reg = Register(
                                static_cast<uint16_t>(observed.fields.seq + 1),
                                static_cast<uint16_t>(state.client_idx),
                                static_cast<uint32_t>(value));
                            for (size_t j = 0; j < state.quorum; ++j) {
                                size_t rj = state.quorum_indices[j];
                                needs_cas[rj] = true;
                                expected[j] = swap_bufs[rj].raw; // Fallback read data safely from prior CAS results
                            }
                            success_count = 0;
                        }
                    }
                }

                if (success_count >= state.quorum) {
#if CHIMERA_CACHE_ENABLED
                    state.cache.put(key, target_reg);
#endif
                    if (measuring) {
                        state.addPutMeasurement(start, std::chrono::steady_clock::now());
                    }
                    step = Done;
                } else {
                    state.put_retries++;
                    postCas();
                }
                break;
            }

            case Done:
                break;
        }
        return true;
    }
};

} // namespace chimera