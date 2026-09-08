// Copyright 2026 PingCAP, Inc.
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

#pragma once

#include <common/types.h>

namespace DB
{
/// Statistics for one hash table owned by a physical hash-table operator.
struct HashTableStats
{
    /// The protocol field is named `ndv`, but its executor-specific meaning is intentionally different:
    /// Join V1 reports distinct hash entries, while Join V2 reports build-side row count because its
    /// pointer table does not maintain a distinct-key count. This is the current by-design behavior.
    UInt64 ndv = 0;
    UInt64 bytes = 0;

    void merge(const HashTableStats & other)
    {
        ndv += other.ndv;
        bytes += other.bytes;
    }
};
} // namespace DB
