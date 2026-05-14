#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <thread>

// Dory / Swarm-KV Headers
#include <lyra/lyra.hpp>
#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>
#include <dory/conn/rc.hpp>
#include <dory/memstore/store.hpp>
#include <dory/shared/match.hpp>

// Chimera Headers (Assuming these are adapted to accept Dory pointers)
#include "client.h"
#include "client_factory.h"
#include "register.h"
#include "metrics.h"
#include "util.h"
#include "exp_cfg.h"
#include "workload.h"

using namespace dory;
using namespace dory::conn;

void run_workload(Client* client, uint64_t i, dory::ProcId id, 
                  uint64_t num_registers,
                  float rq_p, float get_p,
                  ChimeraMetrics& metrics,
                  std::chrono::high_resolution_clock::time_point& start_time,
                  std::chrono::high_resolution_clock::time_point& end_time) {

    uint16_t global_thread_id = id; // Simplified for async Dory execution
    
    // 1. Warmup
    {
        UniformGen uniform(num_registers);
        uniform.seed(global_thread_id + 42);
        auto warmup_deadline = std::chrono::high_resolution_clock::now() + std::chrono::seconds(10);
        while (std::chrono::high_resolution_clock::now() < warmup_deadline) {
            client->get(uniform.next() % num_registers);
        }
    }

    PrefillStream stream(num_registers, rq_p, get_p, static_cast<uint64_t>(global_thread_id) * 6364136223846793005ULL + 1);
    
    metrics.ops_completed = 0;
    const uint64_t ops_to_run = PrefillStream::kSize;

    // Start timed section
    start_time = std::chrono::high_resolution_clock::now();

    for (uint64_t op_idx = 0; op_idx < ops_to_run; ++op_idx) {
        auto& e = stream.next();

        // Optimized path: Execution (Assuming asynchronous futures inside Dory-adapted client)
        if (e.p < rq_p) {
            client->range_query(e.key, std::min(e.key + 9, num_registers - 1));
        } else if (e.p < rq_p + get_p) {
            client->get(e.key);
        } else {
            client->put(e.key, 69);
        }
        metrics.ops_completed++; 
    }

    end_time = std::chrono::high_resolution_clock::now();
    auto thread_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    metrics.duration_sec = thread_duration_ms / 1000.0;
}

int main(int argc, char **argv) {
    ProcId proc_id = 0;
    uint64_t num_clients = 1;
    uint64_t num_servers = 1;
    uint64_t num_registers = 10000;
    float rq_p = 0.0, get_p = 0.5;
    std::string backend = "dory";

    // 1. Argument Parsing (using Lyra like swarm-kv)
    auto cli =
        lyra::cli() |
        lyra::opt(proc_id, "proc_id").required()["-i"]["-p"]["--id"]["--process"].help("ID of this process.") |
        lyra::opt(num_clients, "num_clients").optional()["-c"]["--clients"] |
        lyra::opt(num_servers, "num_servers").optional()["-s"]["--servers"] |
        lyra::opt(num_registers, "num_registers").optional()["-r"]["--regs"] |
        lyra::opt(rq_p, "rq_p").optional()["--rq"] |
        lyra::opt(get_p, "get_p").optional()["--get"];

    auto result = cli.parse({argc, argv});
    if (!result) {
        std::cerr << "Error in command line: " << result.errorMessage() << std::endl;
        return 1;
    }

    auto num_proc = num_clients + num_servers;
    bool is_client = proc_id > num_servers;

    // 2. Resolve Dory Devices & Ports
    ctrl::Devices d;
    ctrl::OpenDevice od;
    auto& available_devices = d.list();
    bool found = false;

    for (auto& dev : available_devices) {
        if (std::string(dev.devName()) == "mlx5_2" || std::string(dev.devName()) == "mlx5_0") {
            od = std::move(dev);
            found = true;
            break;
        }
    }

    if (!found) {
        std::cerr << "Error: IB Device not found!" << std::endl;
        return 1;
    }

    ctrl::ResolvedPort resolved_port(od);
    if (!resolved_port.bindTo(0)) {
        throw std::runtime_error("Couldn't bind the RDMA device.");
    }

    // 3. Configure Control Block & Memory Regions
    ctrl::ControlBlock cb(resolved_port);
    cb.registerPd("primary");

    auto& store = memstore::MemoryStore::getInstance();

    if (is_client) {
        // CLIENT SETUP
        std::cout << "Configuring Client " << proc_id << std::endl;

        // 1. Local scratchpad (Private result area)
        // We only need enough space for 1 cache line (64 bytes) per server connection
        size_t scratch_size = num_servers * 64; 
        cb.allocateBuffer("scratch-buf", scratch_size, 64);
        cb.registerMr("scratch-mr", "primary", "scratch-buf", 
                    ctrl::ControlBlock::LOCAL_READ | ctrl::ControlBlock::LOCAL_WRITE);

    
        // 2. Identify Servers (Clients connect to all servers)
        std::vector<ProcId> server_ids;
        for (ProcId id = 1; id <= num_servers; id++) server_ids.push_back(id);

        for (auto const& id : server_ids) cb.registerCq(fmt::format("cq{}", id));

        RcConnectionExchanger<ProcId> ce(proc_id, server_ids, cb);
        for (auto const& id : server_ids) {
            // Note: "shared-mr" here refers to the MR name ON THE SERVER
            ce.configure(id, "primary", "shared-mr", fmt::format("cq{}", id), fmt::format("cq{}", id));
        }

        // 3. Handshake & Connect
        ce.announceAll(store, "qp");
        ce.announceReady(store, "qp", "prepared");
        ce.waitReadyAll(store, "qp", "prepared");
        ce.connectAll(store, "qp", ctrl::ControlBlock::LOCAL_READ | ctrl::ControlBlock::LOCAL_WRITE); 
        ce.announceReady(store, "qp", "connected");
        ce.waitReadyAll(store, "qp", "connected");
        
        // Prepare the vector of ReplicaConn for the Client
        std::vector<chimera::AsyncCasAbdClient::ReplicaConn> chimera_conns;

        for (ProcId s_id = 1; s_id <= num_servers; s_id++) {
            // 1. Get the remote virtual address from Memstore
            std::string key = fmt::format("replica_addr_{}", s_id);
            std::string addr_str;
            while(!store.get(key, addr_str)) { /* busy wait for server to post addr */ }
            uintptr_t remote_addr = std::stoull(addr_str);

            // 2. Get the established connection from the Exchanger
            // Dory's connectAll populates the connections mapped by ProcId
            auto& conn = ce.getConnection(s_id);

            chimera_conns.push_back({&conn, remote_addr});
        }

        ChimeraMetrics node_metrics;
        std::chrono::high_resolution_clock::time_point start_time, end_time;

        // Pass the Dory ControlBlock and the packaged connections
        auto client = std::make_unique<chimera::AsyncCasAbdClient>(
            static_cast<uint16_t>(proc_id), 
            cb, 
            chimera_conns,
            num_registers, 
            &node_metrics
        );

        ce.announceReady(store, "qp", "initialized");
        ce.waitReadyAll(store, "qp", "initialized");

        // Workload execution (pipelined, non-blocking)
        run_workload(client.get(), 0, proc_id, num_registers, rq_p, get_p, node_metrics, start_time, end_time);

        // Finalize
        client->finishAllFutures(); // Dory specific flush
        node_metrics.Report(proc_id, node_metrics.duration_sec);

        ce.announceReady(store, "qp", "finished");
        ce.waitReadyAll(store, "qp", "finished");
        ce.unannounceReady(store, "qp", "initialized");
        ce.unannounceReady(store, "qp", "finished");
        ce.unannounceReady(store, "qp", "connected");
        ce.unannounceAll(store, "qp");
        ce.unannounceReady(store, "qp", "prepared");

    } else {
        // REPLICA (SERVER) SETUP
        // 1. Setup shared registers
        size_t server_buf_size = num_registers * sizeof(Register);
        cb.allocateBuffer("shared-buf", server_buf_size, 64);
        cb.registerMr("shared-mr", "primary", "shared-buf",
            ctrl::ControlBlock::LOCAL_READ | ctrl::ControlBlock::LOCAL_WRITE |
            ctrl::ControlBlock::REMOTE_READ | ctrl::ControlBlock::REMOTE_WRITE |
            ctrl::ControlBlock::REMOTE_ATOMIC);
            
        memset(reinterpret_cast<void*>(cb.mr("shared-mr").addr), 0, server_buf_size);
        store.set(fmt::format("replica_addr_{}", proc_id), std::to_string(cb.mr("shared-mr").addr));

        // 2. Identify Clients (Servers connect to all clients)
        std::vector<ProcId> client_ids;
        for (ProcId id = num_servers + 1; id <= num_proc; id++) client_ids.push_back(id);

        for (auto const& id : client_ids) cb.registerCq(fmt::format("cq{}", id));

        RcConnectionExchanger<ProcId> ce(proc_id, client_ids, cb);
        for (auto const& id : client_ids) {
            // Servers don't really need a remote MR from clients for ABD
            ce.configure(id, "primary", "scratch-mr", fmt::format("cq{}", id), fmt::format("cq{}", id));
        }

        // 3. Handshake & Connect
        ce.announceAll(store, "qp");
        ce.announceReady(store, "qp", "prepared");
        ce.waitReadyAll(store, "qp", "prepared");
        ce.connectAll(store, "qp", ctrl::ControlBlock::LOCAL_READ | ctrl::ControlBlock::LOCAL_WRITE);
        ce.announceReady(store, "qp", "connected");
        ce.waitReadyAll(store, "qp", "connected");

        std::cout << "Server " << proc_id << " online." << std::endl;
        ce.waitReadyAll(store, "qp", "finished");

        // FIX: Moved cleanup inside the server block
        ce.announceReady(store, "qp", "finished"); // Echo finish so clients can exit safely
        ce.unannounceReady(store, "qp", "initialized");
        ce.unannounceReady(store, "qp", "finished");
        ce.unannounceReady(store, "qp", "connected");
        ce.unannounceAll(store, "qp");
        ce.unannounceReady(store, "qp", "prepared");

    }

    
    std::cout << "Experiment Complete!" << std::endl;
    return 0;
}