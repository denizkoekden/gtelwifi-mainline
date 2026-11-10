// SPDX-License-Identifier: Apache-2.0
#pragma once
// All decoder DSOs in one component-store process share this lease. The
// kernel independently serializes VSP ownership across processes.
extern "C" bool SprdVspAcquireSession();
extern "C" void SprdVspReleaseSession();
