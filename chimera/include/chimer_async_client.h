#pragma once

#include <memory>
#include <vector>

// Dory / Fusee Includes
#include <dory/ctrl/block.hpp>
#include <dory/conn/rc.hpp>
#include <dory/shared/types.hpp>

#include "client.h"             // base interface
#include "cas_abd_async.h"      // async implementation
#include "metrics.h"

class ChimeraAsyncClient : public Client {
public:
    ChimeraAsyncClient(dory::ProcId client_id,
                       dory::ctrl::ControlBlock& cb,
                       dory::conn::RcConnectionExchanger<dory::ProcId>& ce,
                       const std::vector<uint64_t>& replicas,
                       size_t max_range,
                       ChimeraMetrics* metrics)
        // Pass the Dory primitives down to your internal implementation
        : impl_(client_id, cb, ce, replicas, max_range, metrics)
    {}

    uint64_t get(uint64_t key) override {
        // Assuming your pipelined logic will leverage Dory's async CQ polling
        return impl_.get_pipelined(key);
    }

    void put(uint64_t key, uint64_t value) override {
        impl_.put(key, value);
    }

    std::vector<uint64_t> range_query(uint64_t start, uint64_t end) override {
        return impl_.range_query(start, end);
    }

    void set_primary_key(uint64_t key) override {
        impl_.set_primary_key(key);
    }

    void fast_put(uint64_t value) override {
        impl_.fast_put(value);
    }

private:
    chimera::AsyncCasAbdClient impl_;
};