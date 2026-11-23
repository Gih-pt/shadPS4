// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::VideoCoreServer {

// libSceVideoCoreServerInterface definitions
struct OrbisVideoCoreAcquireDecoderResourceParam {
    u32 resource_type;
    u32 max_width;
    u32 max_height;
    u32 stream_spec;
};
static_assert(sizeof(OrbisVideoCoreAcquireDecoderResourceParam) == 0x10);

// Status values the VideoCore server reports to the client through the resource callback.
// Reverse engineered from the client state machine in libSceVideoNativeExtEssential: the decoder
// instance creation only succeeds once the callback has been invoked with status 0 for its
// resource id (it waits up to 5 seconds for it and otherwise releases the resource and fails).
constexpr u32 ORBIS_VIDEO_CORE_RESOURCE_STATUS_GRANTED = 0;

using OrbisVideoCoreCallback = void(PS4_SYSV_ABI*)(u32 res_id, u32 status);

s32 PS4_SYSV_ABI
sceVideoCoreAcquireDecoderResource(const OrbisVideoCoreAcquireDecoderResourceParam* param,
                                   OrbisVideoCoreCallback cb, u32* out_resource_id);

s32 PS4_SYSV_ABI sceVideoCoreReleaseDecoderResource(u32 resource_id);

s32 PS4_SYSV_ABI sceVideoCoreAcquireDecoderResourceForBeWrapper(
    const OrbisVideoCoreAcquireDecoderResourceParam* param, OrbisVideoCoreCallback cb,
    u32* out_resource_id);

s32 PS4_SYSV_ABI sceVideoCoreReleaseDecoderResourceForBeWrapper(u32 resource_id);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::VideoCoreServer
