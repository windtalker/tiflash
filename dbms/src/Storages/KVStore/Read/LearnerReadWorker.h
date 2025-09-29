// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/Logger.h>
#include <Storages/KVStore/Read/LearnerRead.h>
#include <Storages/KVStore/TMTContext.h>
#include <Storages/RegionQueryInfo_fwd.h>
#include <pingcap/kv/LockResolver.h>

#include <memory>
#include <unordered_map>

namespace DB
{
namespace tests
{
class LearnerReadTest;
}

struct LearnerReadStatistics
{
    UInt64 read_index_elapsed_ms = 0;
    UInt64 wait_index_elapsed_ms = 0;

    // num_regions == num_read_index_request + num_cached_read_index + num_stale_read
    UInt64 num_regions = 0;
    //
    UInt64 num_read_index_request = 0;
    UInt64 num_cached_read_index = 0;
    UInt64 num_stale_read = 0;
};

// Container of all unavailable regions info.
// (the class is not thread-safe)
struct UnavailableAndLockRegions
{
    UnavailableAndLockRegions(bool for_batch_cop_, bool is_wn_disagg_read_)
        : batch_cop(for_batch_cop_)
        , is_wn_disagg_read(is_wn_disagg_read_)
    {}

    size_t size() const { return unavailable_region_ids.size() + lock_region_ids.size(); }

    bool empty() const { return unavailable_region_ids.empty() && lock_region_ids.empty(); }

    bool unavailableRegionContains(RegionID region_id) const { return unavailable_region_ids.contains(region_id); }

    void addStatus(RegionID id, RegionException::RegionReadStatus status_, std::string && extra_msg_)
    {
        status = status_;
        unavailable_region_ids.emplace(id);
        extra_msg = std::move(extra_msg_);
    }

    void addRegionLocks(RegionID region_id_, std::vector<LockInfoPtr> & input_region_locks)
    {
        for (size_t i = 0; i < region_locks.size(); i++)
        {
            if (!locks.contains(input_region_locks[i]->lock_version()))
            {
                // if the lock with same version already exists, we don't need to add it again.
                locks.emplace(
                    input_region_locks[i]->lock_version(),
                    std::make_shared<pingcap::kv::Lock>(*input_region_locks[i]));
            }
            region_locks.emplace_back(region_id_, std::move(input_region_locks[i]));
        }
        lock_region_ids.emplace(region_id_);
    }

    void addRegionLockAsUnavailableRegion(RegionID region_id_, LockInfoPtr && region_lock_)
    {
        region_locks.emplace_back(region_id_, std::move(region_lock_));
        unavailable_region_ids.emplace(region_id_);
    }

    void tryThrowRegionException();

    void addRegionWaitIndexTimeout(RegionID region_id, UInt64 index_to_wait, UInt64 current_applied_index);

    String toDebugString() const
    {
        FmtBuffer buffer;
        buffer.append("{unavailable_region_ids=[");
        buffer.joinStr(
            unavailable_region_ids.begin(),
            unavailable_region_ids.end(),
            [](const auto & v, FmtBuffer & f) { f.fmtAppend("{}", v); },
            "|");
        buffer.append("{lock_region_ids=[");
        buffer.joinStr(
            lock_region_ids.begin(),
            lock_region_ids.end(),
            [](const auto & v, FmtBuffer & f) { f.fmtAppend("{}", v); },
            "|");
        buffer.append("] locks=");
        buffer.append("[");
        buffer.joinStr(
            region_locks.begin(),
            region_locks.end(),
            [](const auto & v, FmtBuffer & f) { f.fmtAppend("{}({})", v.first, v.second->DebugString()); },
            "|");
        buffer.append("]}");
        return buffer.toString();
    }

private:
    const bool batch_cop;
    const bool is_wn_disagg_read;

    RegionException::UnavailableRegions unavailable_region_ids;
    RegionException::UnavailableRegions lock_region_ids;
    std::vector<std::pair<RegionID, LockInfoPtr>> region_locks;
    std::unordered_map<UInt64, std::shared_ptr<pingcap::kv::Lock>> locks;
    RegionException::RegionReadStatus status{RegionException::RegionReadStatus::NOT_FOUND};
    std::string extra_msg;
};

using RegionsReadIndexResult = std::unordered_map<RegionID, kvrpcpb::ReadIndexResponse>;

/// LearnerReadWorker is serves all read index requests in a query.
class LearnerReadWorker
{
public:
    LearnerReadWorker(
        MvccQueryInfo & mvcc_query_info_,
        TMTContext & tmt_,
        bool for_batch_cop,
        bool is_wn_disagg_read,
        const LoggerPtr & log_);

    LearnerReadSnapshot buildRegionsSnapshot();

    // Ensure the correctness for reading requests for given regions_snapshot.
    // - Execute read index and get the latest applied indexes from TiKV
    // - Wait until the applied index on this store reach the applied index
    std::tuple<Clock::time_point, Clock::time_point> //
    waitUntilDataAvailable(
        const LearnerReadSnapshot & regions_snapshot,
        UInt64 read_index_timeout_ms,
        UInt64 wait_index_timeout_ms);

    const LearnerReadStatistics & getStats() const { return stats; }
    const UnavailableAndLockRegions & getUnavailableRegions() const { return unavailable_and_lock_regions; }
    void tryThrowRegionException() { unavailable_and_lock_regions.tryThrowRegionException(); }

    friend class tests::LearnerReadTest;

private:
    /// read index relate methods
    std::vector<kvrpcpb::ReadIndexRequest> buildBatchReadIndexReq(
        const RegionTable & region_table,
        const LearnerReadSnapshot & regions_snapshot,
        RegionsReadIndexResult & batch_read_index_result);
    void doBatchReadIndex(
        const std::vector<kvrpcpb::ReadIndexRequest> & batch_read_index_req,
        UInt64 timeout_ms,
        RegionsReadIndexResult & batch_read_index_result);
    void recordReadIndexError(const LearnerReadSnapshot & regions_snapshot, RegionsReadIndexResult & read_index_result);

    RegionsReadIndexResult readIndex(
        const LearnerReadSnapshot & regions_snapshot,
        UInt64 timeout_ms,
        Stopwatch & watch);

    /// wait index relate methods
    void waitIndex(
        const LearnerReadSnapshot & regions_snapshot,
        const RegionsReadIndexResult & batch_read_index_result,
        UInt64 timeout_ms,
        Stopwatch & watch);

private:
    MvccQueryInfo & mvcc_query_info;
    TMTContext & tmt;
    const KVStorePtr & kvstore;
    LoggerPtr log;

    LearnerReadStatistics stats;
    UnavailableAndLockRegions unavailable_and_lock_regions;
};
} // namespace DB
