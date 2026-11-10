// SPDX-License-Identifier: Apache-2.0
#include "SprdVspSession.h"
#include <atomic>
namespace { std::atomic<bool> inUse{false}; }
extern "C" bool SprdVspAcquireSession() { return !inUse.exchange(true); }
extern "C" void SprdVspReleaseSession() { inUse.store(false); }
