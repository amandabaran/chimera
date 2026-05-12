#pragma once

#include <memory>
#include <string>
#include <vector>

// Dory / Fusee Includes
#include <dory/ctrl/block.hpp>
#include <dory/conn/rc.hpp>
#include <dory/shared/types.hpp>

#include "client.h"
#include "metrics.h"

#include "chimera_client.h"
#include "chimera_async_client.h"
#include "chimera_batch_client.h"

class ClientFactory {
public:
    static std::unique_ptr<Client> create(const std::string& backend, 
                                          dory::ProcId client_id,
                                          dory::ctrl::ControlBlock& cb,
                                          dory::conn::RcConnectionExchanger<dory::ProcId>& ce,
                                          const std::vector<uint64_t>& replicas,
                                          size_t max_range,
                                          ChimeraMetrics* metrics) {

        if (backend == "chimera") {
            // Note: Update ChimeraClient constructor similarly when ready
            return std::make_unique<ChimeraClient>(client_id, cb, ce, replicas, max_range, metrics);
        }
        if (backend == "chimera-async") {
            return std::make_unique<ChimeraAsyncClient>(client_id, cb, ce, replicas, max_range, metrics);
        }
        if (backend == "chimera-batch") {
            // Note: Update ChimeraBatchClient constructor similarly when ready
            return std::make_unique<ChimeraBatchClient>(client_id, cb, ce, replicas, max_range, metrics);
        }

        throw std::runtime_error("[ClientFactory] Unknown backend: " + backend);
    }
};