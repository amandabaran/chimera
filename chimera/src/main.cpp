#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <thread>
#include <cstring>
#include <string>
#include <optional>
#include <stdexcept>
#include <algorithm>
#include <functional>

// Dory headers
#include <lyra/lyra.hpp>
#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>
#include <dory/conn/rc.hpp>
#include <dory/memstore/store.hpp>
#include <dory/shared/match.hpp>

// Chimera headers
#include "layout.hpp"
#include "chimera_client.hpp"
#include "op_future.hpp"
#include "register.hpp"

using namespace dory;
using namespace dory::conn;

const uint64_t default_warmup     = 1'000'000;
const uint64_t default_iter_count = 1'000'000;
const uint64_t default_keepwarm   =   500'000;

namespace chimera {
    bool cache_enabled = false;
    bool writeback_enabled = false;
}

// A safe custom deleter struct that avoids compiler attribute mismatches
struct PipeDeleter {
    void operator()(FILE* fp) const {
        if (fp) pclose(fp);
    }
};

// Clean forward declarations to satisfy -Wmissing-declarations
std::unique_ptr<FILE, PipeDeleter> exec(const std::string& cmd);
size_t pseudo_hash(const std::string& str);

std::unique_ptr<FILE, PipeDeleter> exec(const std::string& cmd) {
    auto raw_pipe = popen(cmd.c_str(), "r");
    if (!raw_pipe) {
        throw std::runtime_error("popen() failed!");
    }
    return std::unique_ptr<FILE, PipeDeleter>(raw_pipe);
}

// Simple hash implementation to replicate swarm's key-conflict hazard avoidance checking
size_t pseudo_hash(const std::string& str) {
    size_t hash = 5381;
    for (char const &c : str) {
        hash = ((hash << 5) + hash) + static_cast<size_t>(c);
    }
    return hash;
}

int main(int argc, char** argv) {
    chimera::ProcId proc_id = 0;
    bool show_help = false;

    chimera::Layout layout;
    layout.num_clients       = 1;
    layout.num_servers       = 1;
    layout.async_parallelism = 16;
    layout.num_registers     = 10000;
    layout.max_range         = 10;
    layout.majority          = 0;

    float rq_p = 0.0f, get_p = 0.5f, put_p = 0.5f;
    uint64_t iter_count = default_iter_count;
    uint64_t warmup     = UINT64_MAX;

    std::string ycsb_path = "./YCSB/bin/ycsb.sh";
    std::string workload = "./YCSB/workloads/swarm-workloada";
    bool detailed = true;

    // ─── CLI ────────────────────────────────────────────────────────
    auto cli =
        lyra::cli() |
        lyra::help(show_help) |
        lyra::opt(proc_id, "proc_id")
            .required()["-i"]["-p"]["--id"]["--process"]
            .help("ID of this process.") |
        lyra::opt(layout.num_clients, "num_clients")
            .optional()["-c"]["--clients"] |
        lyra::opt(layout.num_servers, "num_servers")
            .optional()["-s"]["--servers"] |
        lyra::opt(layout.majority, "majority")
            .optional()["-m"]["--majority"] |
        lyra::opt(layout.async_parallelism, "async_parallelism")
            .optional()["-a"]["--async"] |
        lyra::opt(layout.num_registers, "num_registers")
            .optional()["-r"]["--regs"] |
        lyra::opt(layout.max_range, "max_range")
            .optional()["--maxrange"] |
        lyra::opt(rq_p,  "rq_p" ).optional()["--rq"] |
        lyra::opt(get_p, "get_p").optional()["--get"] |
        lyra::opt(put_p, "put_p").optional()["--put"] |
        lyra::opt(workload, "workload").optional()["-w"]["--workload"] |
        lyra::opt(ycsb_path, "ycsb_path").optional()["-y"]["--ycsbpath"] |
        lyra::opt(detailed, "detailed").optional()["-d"]["--detailed"] |
        lyra::opt(iter_count, "iter_count").optional()["-I"]["--iter_count"] |
        lyra::opt(warmup,     "warmup")    .optional()["-W"]["--warmup"] |
        lyra::opt(chimera::cache_enabled, "cache")
            ["--cache"]("Enable or disable the Chimera cache system (1 or 0)") |
        lyra::opt(chimera::writeback_enabled, "writeback")
            ["--writeback"]("Enable or disable writeback in CAS-ABD protocol (1 or 0)");

    auto result = cli.parse({argc, argv});
    if (!result || show_help ) {
        std::cerr << cli << std::endl;
        if (!result) {
            std::cerr << "Error in command line: " << result.errorMessage() << std::endl;
        }
        return 1;
    }

    if (warmup == UINT64_MAX) {
        warmup = iter_count < default_warmup ? iter_count : default_warmup;
    }
    const uint64_t keepwarm           = (iter_count + warmup) / 4;
    const uint64_t start_measurements = warmup;
    const uint64_t stop_measurements  = start_measurements + iter_count;
    const uint64_t total_iter_count   = stop_measurements + keepwarm;

    if (layout.majority == 0) {
        layout.majority = layout.num_servers / 2 + 1;
    }

    auto num_proc  = layout.num_clients + layout.num_servers;
    bool is_client = proc_id > layout.num_servers;

    if (proc_id > num_proc) {
        std::cerr << "Invalid process id: " << proc_id
                  << " > " << num_proc << std::endl;
        return 1;
    }

    if (is_client) {
        std::cout << "Workload: " << workload << std::endl;
    }

    // ─── Device + port ─────────────────────────────────────────────
    ctrl::Devices d;
    ctrl::OpenDevice od;
    auto& available_devices = d.list();
    size_t target_index = 0; // This corresponds to the 3rd device (uverbs2 or mlx5_2)

    if (available_devices.size() > target_index) {
        od = std::move(available_devices[target_index]);
        std::cout << "Selected device: " << od.devName() << std::endl;
    } else {
        std::cerr << "Error: Device index " << target_index << " not available." << std::endl;
        std::cerr << "Available devices: ";
        for (auto const& dev : available_devices) std::cerr << dev.devName() << " ";
        std::cerr << std::endl;
        return 1;
    }

    std::cout << od.name() << " " << od.devName() << " "
            << ctrl::OpenDevice::typeStr(od.nodeType()) << " "
            << ctrl::OpenDevice::typeStr(od.transportType()) << std::endl;

    ctrl::ResolvedPort resolved_port(od);
    auto binded = resolved_port.bindTo(0);
    if (!binded) {
        throw std::runtime_error("Couldn't bind the device.");
    }
    std::cout << "Binded successfully (port_id, port_lid) = ("
                << +resolved_port.portId() << ", " << +resolved_port.portLid()
                << ")" << std::endl;

    // ─── Control block & MR ────────────────────────────────────────
    ctrl::ControlBlock cb(resolved_port);
    cb.registerPd("primary");

    {
        size_t allocated_size = is_client ? layout.clientSize()
                                          : layout.serverSize();
        cb.allocateBuffer("shared-buf", allocated_size, 64);
    }

    cb.registerMr(
        "shared-mr", "primary", "shared-buf",
        ctrl::ControlBlock::LOCAL_READ  | ctrl::ControlBlock::LOCAL_WRITE |
        ctrl::ControlBlock::REMOTE_READ | ctrl::ControlBlock::REMOTE_WRITE |
        ctrl::ControlBlock::REMOTE_ATOMIC);

    std::vector<chimera::ProcId> remote_ids;
    for (chimera::ProcId id = 1; id <= num_proc; id++) {
        if (id == proc_id) continue;
        remote_ids.push_back(id);
    }

    for (auto const& id : remote_ids) {
        cb.registerCq(fmt::format("cq{}", id));
    }

    auto local_region = cb.mr("shared-mr").addr;
    if (is_client) {
        layout.client_local_region = local_region;
    } else {
        std::memset(reinterpret_cast<void*>(local_region), 0, layout.serverSize());
    }

    // ─── Connection exchange ───────────────────────────────────────
    auto& store = memstore::MemoryStore::getInstance();
    dory::conn::RcConnectionExchanger<chimera::ProcId> ce(proc_id, remote_ids, cb);

    for (auto const& id : remote_ids) {
        auto cq = fmt::format("cq{}", id);
        ce.configure(id, "primary", "shared-mr", cq, cq);
    }

    ce.announceAll(store, "qp");
    ce.announceReady(store, "qp", "prepared");
    ce.waitReadyAll(store, "qp", "prepared");
    ce.unannounceReady(store, "qp", "finished");

    ce.connectAll(
        store, "qp",
        ctrl::ControlBlock::LOCAL_READ  | ctrl::ControlBlock::LOCAL_WRITE |
        ctrl::ControlBlock::REMOTE_READ | ctrl::ControlBlock::REMOTE_WRITE |
        ctrl::ControlBlock::REMOTE_ATOMIC);

    ce.announceReady(store, "qp", "connected");
    ce.waitReadyAll(store, "qp", "connected");

    ce.unannounceAll(store, "qp");
    ce.unannounceReady(store, "qp", "prepared");

    if (is_client) {
        std::cout << "Configuring Client " << proc_id << std::endl;
        chimera::ChimeraClient client{layout, ce, proc_id};

        // SWARM PATTERN: First client triggers initial data loading phase
        if (proc_id == (layout.num_servers + 1)) {
            std::cout << "Querying YCSB for the set of initial key-pairs... " << std::flush;
            std::vector<std::pair<std::string, std::string>> inserts = {};

            {
                auto fp = exec(ycsb_path + " load basic -P " + workload + " -s 2> /dev/null");
                char buffer[1024];
                while (fgets(buffer, sizeof(buffer), fp.get()) != nullptr) {
                    std::string line(buffer);
                    if (std::strncmp("INSERT ", line.c_str(), 7) != 0) {
                        continue;
                    }
                    auto keystart = std::string("INSERT usertable ").length();
                    auto keyend = line.find(" [", keystart);
                    auto key = line.substr(keystart, keyend - keystart);

                    auto start = keyend + std::string(" [ field0=").length();
                    auto end = line.length() - std::string(" ]\n").length();
                    auto value = line.substr(start, end - start);

                    inserts.emplace_back(key, value);
                }
            }
            std::cout << "Done." << std::endl;

            std::cout << "Inserting the initial key-pairs... " << std::flush;
            for (size_t kvIndex = 0; kvIndex < inserts.size(); kvIndex++) {
                client.finishAllFutures();
                
                uint64_t target_reg = std::stoull(inserts[kvIndex].first.substr(4)) % layout.num_registers;
                
                uint32_t numeric_value = 0;
                try {
                    std::string val_str = inserts[kvIndex].second;
                    val_str.erase(std::remove_if(val_str.begin(), val_str.end(), 
                                  [](char c) { return !std::isdigit(c); }), val_str.end());
                    
                    if (!val_str.empty()) {
                        numeric_value = static_cast<uint32_t>(std::stoull(val_str) & 0xFFFFFFFF);
                    } else {
                        numeric_value = static_cast<uint32_t>(pseudo_hash(inserts[kvIndex].second) & 0xFFFFFFFF);
                    }
                } catch (...) {
                    numeric_value = static_cast<uint32_t>(pseudo_hash(inserts[kvIndex].second) & 0xFFFFFFFF);
                }
                
                client.getFreeFuture().doPut(target_reg, numeric_value, false);
            }
            client.finishAllFutures();
            std::cout << " Done." << std::endl;
        }

        // ─── DEFINITIONS FOR WORKLOAD E/C SUPPORT ───────────────────────────
        enum class OpType { READ, UPDATE, SCAN };

        struct ChimeraOp {
            OpType type;
            uint64_t target_reg;  // Pre-calculated target key index
            uint32_t value;       // Pre-calculated 32-bit parsed update value
            uint32_t scan_count;  // Pre-calculated range bounds
            size_t hkey;          // Cached hash value for layout hazard checks
        };

        std::cout << "Querying YCSB for the list of operations... " << std::flush;
        std::vector<ChimeraOp> operations = {}; 

        {
            auto fp = exec(ycsb_path + " run basic -P " + workload + " -s 2> /dev/null");
            char buffer[1024];
            while (fgets(buffer, sizeof(buffer), fp.get()) != nullptr) {
                std::string line(buffer);
                if (!(std::strncmp("READ ", line.c_str(), 5))) {
                    auto keystart = std::string("READ usertable ").length();
                    auto keyend = line.find(" [", keystart);
                    auto key = line.substr(keystart, keyend - keystart);

                    uint64_t target_reg = std::stoull(key.substr(4)) % layout.num_registers;
                    size_t hkey = pseudo_hash(key);
                    operations.push_back({OpType::READ, target_reg, 0, 0, hkey});

                } else if (!(std::strncmp("UPDATE ", line.c_str(), 7))) {
                    auto keystart = std::string("UPDATE usertable ").length();
                    auto keyend = line.find(" [", keystart);
                    auto key = line.substr(keystart, keyend - keystart);

                    auto start = keyend + std::string(" [ field0=").length();
                    auto end = line.length() - std::string(" ]\n").length();
                    auto value = line.substr(start, end - start);

                    uint64_t target_reg = std::stoull(key.substr(4)) % layout.num_registers;
                    size_t hkey = pseudo_hash(key);

                    uint32_t update_val = 0;
                    try {
                        std::string val_str = value;
                        val_str.erase(std::remove_if(val_str.begin(), val_str.end(), 
                                      [](char c) { return !std::isdigit(c); }), val_str.end());
                        if (!val_str.empty()) {
                            update_val = static_cast<uint32_t>(std::stoull(val_str) & 0xFFFFFFFF);
                        } else {
                            update_val = static_cast<uint32_t>(pseudo_hash(value) & 0xFFFFFFFF);
                        }
                    } catch (...) {
                        update_val = static_cast<uint32_t>(pseudo_hash(value) & 0xFFFFFFFF);
                    }

                    operations.push_back({OpType::UPDATE, target_reg, update_val, 0, hkey});

                } else if (!(std::strncmp("SCAN ", line.c_str(), 5))) {
                    auto keystart = std::string("SCAN usertable ").length();
                    auto keyend = line.find(" ", keystart);
                    auto key = line.substr(keystart, keyend - keystart);
                    
                    auto countstart = keyend + 1;
                    auto countend = line.find(" [", countstart);
                    uint32_t count = static_cast<uint32_t>(std::stoull(line.substr(countstart, countend - countstart)) & 0xFFFFFFFF);

                    uint64_t target_reg = std::stoull(key.substr(4)) % layout.num_registers;
                    size_t hkey = pseudo_hash(key);

                    operations.push_back({OpType::SCAN, target_reg, 0, count, hkey});
                }
            }
        }
        std::cout << "Done. Parsed " << operations.size() << " operations." << std::endl;

        std::cout << "Waiting for the initialization of other clients... " << std::flush;
        ce.announceReady(store, "qp", "initialized");
        ce.waitReadyAll(store, "qp", "initialized");
        std::cout << "Done." << std::endl;

        std::cout << "Running the benchmark (YCSB Swarm Engine)... " << std::endl;

        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point end_time;
        bool measuring = false;
        size_t skipped = 0;

        // ─── MAIN BENCHMARK LOOP (OPTIMIZED) ────────────────────────────────
        for (size_t i = 0; i < total_iter_count; i++) {
            
            retry_next_key:
            auto& op = operations[(i + skipped) % operations.size()];

            // Concurrent pipeline hazard tracking
            if (layout.async_parallelism > 1) {
                size_t hkey = op.hkey; // Read directly from optimized struct layout
                // (Keep any custom pipeline hazard tracking logic from your original build here)
            }

            auto& future = client.getFreeFuture();

            if (i == start_measurements) {
                measuring = true;
                start_time = std::chrono::steady_clock::now();
            } else if (i == stop_measurements) {
                measuring = false;
                end_time = std::chrono::steady_clock::now();
            }

            if (op.type == OpType::UPDATE) {
                future.doPut(op.target_reg, op.value, measuring);
            } 
            else if (op.type == OpType::READ) {
                future.doGet(op.target_reg, measuring);
            } 
            else if (op.type == OpType::SCAN) {
                uint64_t valid_count = op.scan_count;
                if (op.target_reg + valid_count > layout.num_registers) {
                    valid_count = layout.num_registers - op.target_reg;
                }
                if (valid_count > layout.max_range) {
                    valid_count = layout.max_range;
                }
                
                if (valid_count > 0) {
                    future.doRange(op.target_reg, static_cast<uint32_t>(valid_count), measuring);
                } else {
                    future.doGet(op.target_reg, measuring);
                }
            }
            client.getState().ops_completed++;
        }
        
        client.finishAllFutures();
        std::cout << "Done. Results:" << std::endl;

        client.reportStats(detailed);
        fmt::print("Local tput: {} kops\n",
                iter_count * 1'000'000
                / static_cast<uint64_t>((end_time - start_time).count()));

        ce.announceReady(store, "qp", "finished");
        ce.waitReadyAll(store, "qp", "finished");
        ce.unannounceReady(store, "qp", "initialized");

    } else {
        std::cout << "Server " << proc_id << " online." << std::endl;
        ce.announceReady(store, "qp", "initialized");
        ce.announceReady(store, "qp", "finished");
        ce.waitReadyAll(store, "qp", "finished");
        ce.unannounceReady(store, "qp", "initialized");
        std::cout << "Closing server connection." << std::endl;
    }

    ce.unannounceReady(store, "qp", "connected");
    std::cout << "###DONE###" << std::endl;
    return 0;
}