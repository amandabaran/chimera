#include <cstddef>
#pragma once

#include <dory/conn/rc-connection.hpp>
#include <dory/ctrl/control-block.hpp>

#include <unordered_map>
#include <vector>
#include <algorithm>
#include <memory>
#include <cstdint>

#include "cache.h"
#include "client.h"
#include "register.h"
#include "metrics.h"


namespace chimera {

class AsyncCasAbdClient {
private:
    // Dory Framework components
    dory::ctrl::ControlBlock& cb_;
    
    struct ReplicaConn {
        dory::conn::ReliableConnection* rc;
        uintptr_t remote_buf_addr; // Base address of the remote registered MR
    };
    std::vector<ReplicaConn> replica_conns_;

    // Quorum and local state
    uint64_t quorum_;
    uint8_t client_id_;
    ChimeraMetrics* metrics_;
    std::vector<size_t> quorum_indices_;
    std::vector<Register*> replica_scratchpads_; // For CAS and single reads (High speed, no false sharing)
    std::vector<Register*> bulk_scratchpads_; // For Bulk Reads/Range Queries (Packed for iteration)
    uint64_t max_range_len_;
    uint64_t primary_key_; // For fast_put optimization
    Register last_reg_; // For fast_put optimization

    // Internal Dory completion polling
    static thread_local std::vector<struct ibv_wc> wces;
    
    static constexpr size_t MAX_REPLICAS = 7;
    static constexpr size_t PER_CLIENT_MEM_SIZE = 4096; // For CAS and bulk ops

#if CHIMERA_CACHE_ENABLED
    Cache cache_;
    static const size_t FETCH_COUNT = 8;  // Runtime decision
    
#else
    static const size_t FETCH_COUNT = 1;
#endif
    
 // Metric counting functions
    inline void count_read() {
#if CHIMERA_METRICS_ENABLED
        metrics_->rdma_reads++;
#endif
    }
    
    inline void count_cas_attempt() {
#if CHIMERA_METRICS_ENABLED
        metrics_->rdma_cas_attempts++;
#endif
    }
    
    inline void count_cas_fail() {
#if CHIMERA_METRICS_ENABLED
        metrics_->rdma_cas_failures++;
#endif
    }

    inline void count_cache_hit() {
#if CHIMERA_METRICS_ENABLED
        metrics_->cache_hits++;
#endif
    }

    inline void count_cache_miss() {
#if CHIMERA_METRICS_ENABLED
        metrics_->cache_misses++;
#endif
    }

    inline void count_speculation_success() {
#if CHIMERA_METRICS_ENABLED
        metrics_->speculation_success++;
#endif
    }

    inline void count_speculation_fail() {
#if CHIMERA_METRICS_ENABLED
        metrics_->speculation_fail++;
#endif
    }

    inline void update_local_cache(uint64_t key, Register reg) {
#if CHIMERA_CACHE_ENABLED
        cache_.put(key, reg);
#endif
    }
    
    void poll_until_done(int& pending, const std::vector<size_t>& active_replicas) {
        if (wces.capacity() < 16) wces.reserve(16); 
        
        while (pending > 0) {
            for (size_t r : active_replicas) {
                auto& rc = *replica_conns_[r].rc;
                // pollCqIsOk populates the wces vector
                if (rc.pollCqIsOk(dory::conn::ReliableConnection::SendCq, wces)) {
                    pending -= static_cast<int>(wces.size());
                    if (pending <= 0) return;
                }
            }
            _mm_pause();
        }
    }

    void update_quorum_parallel(uint64_t key, const uint64_t* observed_expected, 
                            Register target) {
        uint64_t success_count = 0;
        std::vector<size_t> active_indices;
        uint64_t expected[MAX_REPLICAS];
        
        for (size_t i = 0; i < quorum_; ++i) {
            active_indices.push_back(i);
            expected[i] = observed_expected[i];
        }

        while (success_count < quorum_ && !active_indices.empty()) {
            int cas_pending = 0;
            std::vector<size_t> active_replicas;
            
            for (size_t i : active_indices) {
                size_t r = quorum_indices_[i];
                active_replicas.push_back(r);
                
                // 1. Calculate the actual remote address for this key
                uintptr_t remote_addr = replica_conns_[r].remote_buf_addr + (key * sizeof(Register));
                
                // 2. Pass the RAW uint64_t values for expected and target
                replica_conns_[r].rc->postCas(
                    0, 
                    reinterpret_cast<void*>(replica_scratchpads_[r]), 
                    remote_addr, 
                    expected[i], // Must be the specific uint64_t for this replica
                    target.raw   // Must be the raw uint64_t value
                );
                cas_pending++;
                count_cas_attempt();
            }
            
            poll_until_done(cas_pending, active_replicas);
            
            auto it = active_indices.begin();
            while (it != active_indices.end()) {
                size_t i = *it;
                size_t r = quorum_indices_[i];
                
                // 3. DEREFERENCE the pointer to get the value the NIC just wrote
                Register observed = *replica_scratchpads_[r]; 
                
                if (observed.raw == expected[i]) {
                    success_count++;
                    it = active_indices.erase(it);
                    if (success_count >= quorum_) break;
                } else {
                    count_cas_fail();
                    expected[i] = observed.raw; // Update expected for the next retry
                    ++it;
                }
            }
        }
    }
    
    bool update_quorum_speculative(uint64_t key, Register expected_reg, 
                               Register target_reg, uint64_t* next_expected, 
                               Register& next_target_reg) {
        int pending = 0;
        std::vector<size_t> active_replicas;
        
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            active_replicas.push_back(r);
            
            // 1. Calculate the actual remote address
            uintptr_t remote_addr = replica_conns_[r].remote_buf_addr + (key * sizeof(Register));

            // 2. Fix: Use expected_reg.raw and target_reg.raw
            // 3. Fix: Pass replica_scratchpads_[r] (the pointer) without the extra '&'
            replica_conns_[r].rc->postCas(
                0, 
                reinterpret_cast<void*>(replica_scratchpads_[r]), 
                remote_addr, 
                expected_reg.raw, // Use the argument passed to the function
                target_reg.raw    // Use the argument passed to the function
            );
            pending++;
            count_cas_attempt();
        }
        
        poll_until_done(pending, active_replicas);

        uint64_t success_count = 0;
        uint16_t z_max = 0;
        
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            
            // 4. Fix: Dereference the scratchpad pointer to get the value
            Register observed = *replica_scratchpads_[r];
            
            if (observed.raw == expected_reg.raw) {
                success_count++;
                next_expected[i] = target_reg.raw;
                z_max = std::max(z_max, (uint16_t)target_reg.fields.seq);
            } else {
                count_cas_fail();
                next_expected[i] = observed.raw;
                z_max = std::max(z_max, (uint16_t)observed.fields.seq);
            }
        }

        if (success_count >= quorum_) {
            count_speculation_success();
            return true;
        }
        
        count_speculation_fail();
        // Use target_reg.fields.value to maintain the value being written
        next_target_reg = Register(z_max + 1, client_id_, target_reg.fields.value);
        return false;
    }

public:
    AsyncCasAbdClient(uint16_t client_id,
                      dory::ctrl::ControlBlock& cb,
                      std::vector<ReplicaConn> conns, // Passed from your main.cpp setup
                      size_t max_range,
                      ChimeraMetrics* metrics) 
        : cb_(cb), replica_conns_(conns), client_id_(client_id), 
          max_range_len_(max_range), metrics_(metrics)
#if CHIMERA_CACHE_ENABLED
          , cache_(max_range)
#endif 
    {
#ifdef CHIMERA_SINGLE_REPLICA
        REMUS_INFO("Using single replica mode for benchmarking");
        quorum_ = 1;
#else
        quorum_ = (replicas_.size() / 2) + 1;
#endif
        
        
        // Select quorum replicas for load balancing
        for (size_t i = 0; i < quorum_; ++i) {
            quorum_indices_.push_back((client_id_ + i) % replica_conns_.size());
        }
        // Setup scratchpads within the registered Dory buffer
        size_t per_client_offset = client_id_ * PER_CLIENT_MEM_SIZE; 
        uint8_t* base_ptr = reinterpret_cast<uint8_t*>(cb_.mr("scratch-mr").addr) + per_client_offset;
        for (size_t i = 0; i < replica_conns_.size(); ++i) {
            // 1. Give each replica 64 bytes of "private" space for CAS
            replica_scratchpads_.push_back(
                reinterpret_cast<Register*>(base_ptr + (i * 64))
            );

            // 2. Give each replica a larger, PACKED space for bulk ops
            // This starts after all the CAS scratchpads
            uintptr_t bulk_start = replica_conns_.size() * 64; 
            bulk_scratchpads_.push_back(
                reinterpret_cast<Register*>(base_ptr + bulk_start + (i * max_range_len_ * sizeof(Register)))
            );
        }
        
        REMUS_INFO("AsyncCasAbdClient {} initialized with quorum={}", 
                client_id_, quorum_);
    }

    ~AsyncCasAbdClient() {}
    uint64_t get_pipelined(uint64_t key) {
        int pending = 0;
        std::vector<size_t> active_replicas;
        
        // Issue reads
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            active_replicas.push_back(r);
            auto &rc = replica_conns_[r];
            
            replica_conns_[r].rc->postSendSingle(
                dory::conn::ReliableConnection::RdmaRead, 
                0, // wr_id (can be used to track specific ops)
                reinterpret_cast<void*>(&replica_scratchpads_[r][0]), // Local addr
                sizeof(uint64_t),                                     // Length
                replica_conns_[r].remote_buf_addr + (key * sizeof(Register)) // Remote addr
            );
            pending++;
        }
        
        // OPTIMIZATION: Poll opportunistically while waiting
        // Check a few times to see if any completions arrived early
        int completions = 0;
        for (int quick_check = 0; quick_check < 3 && completions < quorum_; ++quick_check) {
            for (size_t r : active_replicas) {
                int got = remus::internal::PollCompletionsFast(replica_conns_[r].conn, 1);
                completions += got;
            }
        }
        
        // Now poll until all complete (most should already be done)
        poll_until_done(pending, active_replicas);
        
        // Start computing max BEFORE deciding on writeback
        Register max_reg(0);
        uint64_t max_counts = 0;
        uint64_t expected[MAX_REPLICAS];
        
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            Register reg = replica_scratchpads_[r][0];
            expected[i] = reg.raw;
            count_read();
            
            if (max_reg < reg) {
                max_reg = reg;
                max_counts = 1;
            } else if (reg == max_reg) {
                max_counts++;
            }
        }
        
#if CHIMERA_WRITEBACK_ENABLED
        if (max_counts < quorum_) {
            metrics_->gets_with_writeback++;
            update_quorum_parallel(key, expected, max_reg);
        } else {
            metrics_->gets_without_writeback++;
        }
#else
        metrics_->gets_without_writeback++;  // No writeback enabled
#endif
        
        return max_reg.fields.value;
    }

    uint64_t get(uint64_t key) {
#if CHIMERA_CACHE_ENABLED
        printf("CACHE IS ENABLED!\n");
        uint64_t base_key = (key / FETCH_COUNT) * FETCH_COUNT;
        
        int pending = 0;  // Regular int, no atomic!
        std::vector<size_t> active_replicas;
        
        // Issue bulk reads to quorum
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            active_replicas.push_back(r);
            auto &rc = replica_conns_[r];

            size_t bytes = FETCH_COUNT * sizeof(Register);
            replica_conns_[r].rc->postSendSingle(
                dory::conn::ReliableConnection::RdmaRead, 
                0, // wr_id (can be used to track specific ops)
                reinterpret_cast<void*>(&bulk_scratchpads_[r][0]), // Local addr
                bytes,                                     // Length
                replica_conns_[r].remote_buf_addr + (key * sizeof(Register)) // Remote addr
            );
            pending++;
        }
        
        poll_until_done(pending, active_replicas);
        
        // Compute max for all fetched keys
        Register requested_key_max(0);
        uint64_t requested_key_count = 0;
        uint64_t req_expected[MAX_REPLICAS];

        for (size_t i = 0; i < FETCH_COUNT; ++i) {
            Register max_reg_for_idx(0);
            uint64_t max_count_for_idx = 0;

            for (size_t k = 0; k < quorum_; ++k) {
                size_t r = quorum_indices_[k];
                Register reg = bulk_scratchpads_[r][i];
                count_read();
                
                if (max_reg_for_idx < reg) {
                    max_reg_for_idx = reg;
                    max_count_for_idx = 1;
                } else if (reg == max_reg_for_idx) {
                    max_count_for_idx++;
                }
            }

            uint64_t current_key = base_key + i;
            if (current_key < max_range_len_) {
                update_local_cache(current_key, max_reg_for_idx);
                
                if (current_key == key) {
                    requested_key_max = max_reg_for_idx;
                    requested_key_count = max_count_for_idx;
                    for (size_t k = 0; k < quorum_; ++k) {
                        size_t r = quorum_indices_[k];
                        req_expected[k] = bulk_scratchpads_[r][i].raw;
                    }
                }
            }
        }

#if CHIMERA_WRITEBACK_ENABLED
        if (max_counts < quorum_) {
            metrics_->gets_with_writeback++;  // ← Use metrics_
            update_quorum_parallel(key, expected, max_reg);
        } else {
            metrics_->gets_without_writeback++;
        }
#else
        metrics_->gets_without_writeback++;  // No writeback enabled
#endif
        
        return requested_key_max.fields.value;
#else
        printf("CACHE IS NOT ENABLED!\n");
        int pending = 0;
        std::vector<size_t> active_replicas;
        
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            active_replicas.push_back(r);
            auto &rc = replica_conns_[r];
            
            replica_conns_[r].rc->postSendSingle(
                dory::conn::ReliableConnection::RdmaRead, 
                0, // wr_id (can be used to track specific ops)
                reinterpret_cast<void*>(&replica_scratchpads_[r][0]), // Local addr
                sizeof(uint64_t),                                     // Length
                replica_conns_[r].remote_buf_addr + (key * sizeof(Register)) // Remote addr
            );
            pending++;
        }
        
        poll_until_done(pending, active_replicas);
        
        Register max_reg(0);
        uint64_t max_counts = 0;
        uint64_t expected[MAX_REPLICAS];
        
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            Register reg = replica_scratchpads_[r][0];
            expected[i] = reg.raw;
            count_read();
            
            if (max_reg < reg) {
                max_reg = reg;
                max_counts = 1;
            } else if (reg == max_reg) {
                max_counts++;
            }
        }
        
#if CHIMERA_WRITEBACK_ENABLED
        if (max_counts < quorum_) {
            metrics_->gets_with_writeback++;  // ← Use metrics_
            update_quorum_parallel(key, expected, max_reg);
        } else {
            metrics_->gets_without_writeback++;
        }
#else
        metrics_->gets_without_writeback++;  // No writeback enabled
#endif
        
        return max_reg.fields.value;
#endif
    }

    void put(uint64_t key, uint64_t val) {
#if CHIMERA_CACHE_ENABLED
        printf("PUT: CACHE IS ENABLED!\n");
        Register cached_reg(0);
        if (cache_.get(key, cached_reg)) {
            count_cache_hit();
            Register target_reg(cached_reg.fields.seq + 1, client_id_, val);
            uint64_t next_expected[MAX_REPLICAS];
            Register next_target(0);

            if (update_quorum_speculative(key, cached_reg, target_reg, 
                                         next_expected, next_target)) {
                update_local_cache(key, target_reg);
                return;
            }
            
            update_quorum_parallel(key, next_expected, next_target);
            update_local_cache(key, next_target);
            return;
        }
        count_cache_miss();
#endif
        printf("PUT: CACHE IS NOT ENABLED!\n");
        int pending = 0;
        std::vector<size_t> active_replicas;
        
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            active_replicas.push_back(r);
            auto &rc = replica_conns_[r];
 
            replica_conns_[r].rc->postSendSingle(
                dory::conn::ReliableConnection::RdmaRead, 
                0, // wr_id (can be used to track specific ops)
                reinterpret_cast<void*>(&replica_scratchpads_[r][0]), // Local addr
                sizeof(uint64_t),                                     // Length
                replica_conns_[r].remote_buf_addr + (key * sizeof(Register)) // Remote addr
            );
            pending++;
        }
        
        poll_until_done(pending, active_replicas);
        
        uint16_t z_max = 0;
        uint64_t expected[MAX_REPLICAS];
        
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            Register local_reg = replica_scratchpads_[r][0];
            expected[i] = local_reg.raw;
            count_read();
            z_max = std::max(z_max, (uint16_t)local_reg.fields.seq);
        }

        Register new_reg(z_max + 1, client_id_, val);
        update_quorum_parallel(key, expected, new_reg);
        update_local_cache(key, new_reg);
    }

    std::vector<uint64_t> range_query(uint64_t start_key, uint64_t end_key) {
        size_t range_len = (end_key - start_key) + 1;
        
        int pending = 0;
        std::vector<size_t> active_replicas;  
        
        // Issue bulk reads to quorum
        for (size_t i = 0; i < quorum_; ++i) {
            size_t r = quorum_indices_[i];
            active_replicas.push_back(r);
            auto &rc = replica_conns_[r];
            
            size_t bytes = range_len * sizeof(Register);
            replica_conns_[r].rc->postSendSingle(
                dory::conn::ReliableConnection::RdmaRead, 
                0, // wr_id (can be used to track specific ops)
                reinterpret_cast<void*>(&bulk_scratchpads_[r][0]), // Local addr
                bytes,                                     // Length
                replica_conns_[r].remote_buf_addr + (start_key * sizeof(Register)) // Remote addr
            );
            pending++;
        }
        
        // Poll until all reads complete
        poll_until_done(pending, active_replicas);
        
        // Compute max for each key in the range
        std::vector<Register> max_registers(range_len, Register(0));
        std::vector<uint64_t> max_counts(range_len, 0);

        for (size_t i = 0; i < range_len; ++i) {
            for (size_t k = 0; k < quorum_; ++k) {
                size_t r = quorum_indices_[k];
                Register reg = bulk_scratchpads_[r][i];
                count_read();
                
                if (max_registers[i] < reg) {
                    max_registers[i] = reg;
                    max_counts[i] = 1;
                } else if (reg == max_registers[i]) {
                    max_counts[i]++;
                }
            }
        }

        // Extract values
        std::vector<uint64_t> results(range_len);
        for (size_t i = 0; i < range_len; ++i) {
            results[i] = max_registers[i].fields.value;
            
            // Optional: Write-back if max not on quorum
            // if (max_counts[i] < quorum_) {
            //     uint64_t expected[MAX_REPLICAS];
            //     for (size_t k = 0; k < quorum_; ++k) {
            //         size_t r = quorum_indices_[k];
            //         expected[k] = bulk_scratchpads_[r][i].raw;
            //     }
            //     update_quorum_parallel(start_key + i, expected, max_registers[i]);
            // }
        }
        
        return results;
    }

    void set_primary_key(uint64_t key) { 
        primary_key_ = key; 
    }

    void fast_put(uint64_t val) {
        Register new_reg(last_reg_.fields.seq + 1, client_id_, val);
        uint64_t expected[MAX_REPLICAS];
        for (size_t i = 0; i < quorum_; ++i) expected[i] = last_reg_.raw;
        update_quorum_parallel(primary_key_, expected, new_reg);
        last_reg_ = new_reg;
    }
};

thread_local std::vector<struct ibv_wc> AsyncCasAbdClient::wces;

} // namespace chimera