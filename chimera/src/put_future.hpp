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
        OptimisticChain, // Cache-hit path: doorbell-batched READ+CAS chain
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

    // ─── Optimistic path: doorbell-chained READ + CAS per server ─────
    //
    // wr[0] = RDMA READ  (unsignaled) → fills read_bufs[r] for fallback
    // wr[1] = ATOMIC CAS (signaled)   → tries to install target_reg using
    //                                    the cached expected value
    //
    // 1 doorbell, 1 CQE per server.  If the CAS succeeds, we're done in
    // a single RTT.  If it fails, read_bufs[r] now holds the current value
    // and swap_bufs[r] holds it too (CAS swapback).  We fall back to
    // the regular CAS retry phase.
    void postOptimisticChain(size_t server_idx) {
        auto& rc = *state.server_conns[server_idx];
        uintptr_t remote = Layout::remoteAddrOf(rc.remoteBuf(), key);

        struct ibv_send_wr wr[2];
        struct ibv_sge     sg[2];

        rc.prepareSingle(
            wr[0], sg[0],
            dory::conn::ReliableConnection::RdmaRead,
            future_id,
            &read_bufs[server_idx],
            sizeof(Register),
            remote,
            /*signaled=*/false);

        rc.prepareSingleCas(
            wr[1], sg[1],
            future_id,
            &swap_bufs[server_idx],
            remote,
            expected[server_idx],   // from cache
            target_reg.raw,
            /*signaled=*/true);

        wr[0].next = &wr[1];
        rc.postSend(wr[0]);

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
            // ─── Cache-hit fast path: optimistic doorbell-batched chain ───
            state.metrics_cache_hit();   // or your hook

            // Build target = (cached.seq + 1, client_id, value)
            target_reg = Register(
                static_cast<uint16_t>(cached.fields.seq + 1),
                static_cast<uint16_t>(state.client_idx),
                static_cast<uint32_t>(value));

            // Same expected for every server — what we think the slot has
            for (size_t i = 0; i < state.layout.num_servers; ++i) {
                expected[i] = cached.raw;
            }
            for (size_t i = 0; i < state.quorum; ++i) {
                size_t r = state.quorum_indices[i];
                needs_cas[r] = true;
                postOptimisticChain(r);
            }
            step = OptimisticChain;
            return;
        }
        state.metrics_cache_miss();
#endif

        // ─── Cache-miss path: original two-phase READ + CAS ──────────
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
            // ─── Phase 1 of cache-miss path ─────────────────────────
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

            // ─── Optimistic chain completed ─────────────────────────
            case OptimisticChain: {
                // For each server: did the CAS succeed?
                //   Yes → swap_bufs[r].raw == expected[r] (the cached value)
                //   No  → swap_bufs[r].raw  = actual current value at that slot
                //         (read_bufs[r] holds the same; it's a free check)
                uint16_t z_max = target_reg.fields.seq - 1;  // start at our base
                bool any_failed = false;

                for (size_t i = 0; i < state.quorum; ++i) {
                    size_t r = state.quorum_indices[i];
                    Register observed = swap_bufs[r];
                    state.countRead();   // chained READ also counts

                    if (observed.raw == expected[r]) {
                        // Optimistic CAS succeeded
                        needs_cas[r] = false;
                        success_count++;
                    } else {
                        // CAS failed — observed is the real current value
                        state.countCasFail();
                        any_failed = true;
                        // Re-key expected[] to per-quorum-slot indexing for Phase 2
                        // (Note: in optimistic path we used expected[r] keyed by server.
                        //  We rewrite to expected[i] keyed by quorum-slot here.)
                        expected[i] = observed.raw;
                        z_max = std::max(z_max, observed.fields.seq);
                    }
                }

                // Also normalize succeeded entries' expected[i] to quorum-slot keying
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

                // Some optimistic CASes failed.  Bump our seq above the highest
                // observed and retry only the laggards.
                state.put_retries++;
                target_reg = Register(
                    static_cast<uint16_t>(z_max + 1),
                    static_cast<uint16_t>(state.client_idx),
                    static_cast<uint32_t>(value));

                postCas();
                step = CAS;
                break;
            }

            // ─── Phase 2: CAS retry loop ────────────────────────────
            case CAS: {
                bool any_failed = false;

                for (size_t i = 0; i < state.quorum; ++i) {
                    size_t r = state.quorum_indices[i];
                    if (!needs_cas[r]) continue;

                    Register observed = swap_bufs[r];
                    if (observed.raw == expected[i]) {
                        // CAS succeeded
                        needs_cas[r] = false;
                        success_count++;
                    } else {
                        state.countCasFail();
                        any_failed = true;
                        expected[i] = observed.raw;

                        // If observed has higher-or-equal seq than our target,
                        // bump our seq above it and reset every replica's CAS.
                        if (observed.fields.seq >= target_reg.fields.seq) {
                            target_reg = Register(
                                static_cast<uint16_t>(observed.fields.seq + 1),
                                static_cast<uint16_t>(state.client_idx),
                                static_cast<uint32_t>(value));
                            // Previously-succeeded replicas now have a stale
                            // value too — redo all of them.
                            for (size_t j = 0; j < state.quorum; ++j) {
                                size_t rj = state.quorum_indices[j];
                                needs_cas[rj] = true;
                                expected[j] = read_bufs[rj].raw;  // best known
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
                    // step stays CAS
                }
                break;
            }

            case Done:
                break;
            default:
                break;
        }
        return true;
    }
};

} // namespace chimera
