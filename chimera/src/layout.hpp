#pragma once

#include <cstdint>
#include <cstddef>
#include "register.hpp"

namespace chimera {

using ProcId = uint64_t;

struct Layout {
    // Configurable via CLI (set in main.cpp)
    uint64_t num_clients = 1;
    uint64_t num_servers = 1;
    uint64_t async_parallelism = 16;
    uint64_t num_registers = 10000;
    uint64_t max_range = 10;
    uint64_t majority = 0;       // 0 = auto-compute (num_servers/2 + 1)

    // Set by client at runtime after MR is allocated.
    // (Same pattern as swarm-kv: see Layout::client_local_region)
    uintptr_t client_local_region = 0;

    uint64_t firstClientId() const { return num_servers + 1; }

    uint64_t effectiveMajority() const {
        return majority == 0 ? (num_servers / 2 + 1) : majority;
    }

    // ─── Per-future scratchpad layout (in client's MR) ─────────────────
    // Each future owns a contiguous region:
    //   [read_bufs : num_servers * sizeof(Register)]
    //   [swap_bufs : num_servers * sizeof(Register)]
    //   [bulk_bufs : num_servers * max_range * sizeof(Register)]

    size_t readBufsSize() const { return num_servers * sizeof(Register); }
    size_t swapBufsSize() const { return num_servers * sizeof(Register); }
    size_t bulkBufsSize() const { return num_servers * max_range * sizeof(Register); }
    size_t perFutureSize() const {
        return readBufsSize() + swapBufsSize() + bulkBufsSize();
    }

    size_t clientSize() const { return async_parallelism * perFutureSize(); }
    size_t serverSize() const { return num_registers * sizeof(Register); }

    // ─── Pointer accessors (use client_local_region) ───────────────────
    Register* getReadBufs(uint64_t future_id) const {
        return reinterpret_cast<Register*>(
            client_local_region + future_id * perFutureSize());
    }
    Register* getSwapBufs(uint64_t future_id) const {
        return reinterpret_cast<Register*>(
            client_local_region + future_id * perFutureSize() + readBufsSize());
    }
    Register* getBulkBufs(uint64_t future_id) const {
        return reinterpret_cast<Register*>(
            client_local_region + future_id * perFutureSize()
            + readBufsSize() + swapBufsSize());
    }

    // ─── Remote address calculation ────────────────────────────────────
    static uintptr_t remoteAddrOf(uintptr_t remote_base, uint64_t key) {
        return remote_base + key * sizeof(Register);
    }
};

} // namespace chimera