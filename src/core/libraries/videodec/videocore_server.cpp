// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/libs.h"
#include "core/libraries/videodec/videocore_server.h"
#include "core/libraries/videodec/videodec_error.h"

namespace Libraries::VideoCoreServer {

struct VideoCoreResource {
    u32 id;
    bool is_be_wrapper;
    OrbisVideoCoreAcquireDecoderResourceParam param;
    OrbisVideoCoreCallback callback;
};

// The real VideoCore server lives in another process and reports resource events back to the
// client asynchronously. Clients register their decoder instance only after the acquire call
// returns, so the event must never be delivered from inside the acquire call itself. This models
// the latency of that round trip.
constexpr std::chrono::milliseconds EVENT_DELIVERY_LATENCY{10};

struct VideoCoreEvent {
    u32 id;
    OrbisVideoCoreCallback callback;
    u32 status;
    std::chrono::steady_clock::time_point deliver_at;
};

class VideoCoreResourceManager {
public:
    static VideoCoreResourceManager& Instance() {
        // Intentionally leaked: the event thread is a guest thread that can't be joined safely
        // during static destruction.
        static auto* instance = new VideoCoreResourceManager();
        return *instance;
    }

    s32 AcquireResource(const OrbisVideoCoreAcquireDecoderResourceParam* param,
                        OrbisVideoCoreCallback cb, u32* out_resource_id, bool is_be_wrapper) {
        if (!param || !cb || !out_resource_id) {
            LOG_ERROR(Lib_Videodec, "Invalid arguments passed to VideoCore acquire");
            return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
        }

        std::unique_lock lock(mutex);
        const u32 id = next_id++;
        resources[id] = VideoCoreResource{
            .id = id,
            .is_be_wrapper = is_be_wrapper,
            .param = *param,
            .callback = cb,
        };
        *out_resource_id = id;

        LOG_INFO(Lib_Videodec,
                 "Acquired VideoCore {} decoder resource id={}, res_type={}, dim={}x{}, spec={}",
                 is_be_wrapper ? "BeWrapper" : "Standard", id, param->resource_type,
                 param->max_width, param->max_height, param->stream_spec);

        // Tell the client the resource is now usable.
        events.push_back(VideoCoreEvent{
            .id = id,
            .callback = cb,
            .status = ORBIS_VIDEO_CORE_RESOURCE_STATUS_GRANTED,
            .deliver_at = std::chrono::steady_clock::now() + EVENT_DELIVERY_LATENCY,
        });
        if (!event_thread.Joinable()) {
            event_thread.Run([this](const std::stop_token& stop) { EventThread(stop); });
        }
        event_cv.notify_one();

        return ORBIS_OK;
    }

    s32 ReleaseResource(u32 resource_id, bool is_be_wrapper) {
        LOG_INFO(Lib_Videodec, "Releasing VideoCore {} resource_id={}",
                 is_be_wrapper ? "BeWrapper" : "Standard", resource_id);

        if (resource_id == 0) {
            return is_be_wrapper ? ORBIS_VIDEO_CORE_ERROR_BE_WRAPPER_RESOURCE_NOT_FOUND
                                 : ORBIS_VIDEO_CORE_ERROR_RESOURCE_NOT_FOUND;
        }

        std::unique_lock lock(mutex);
        auto it = resources.find(resource_id);
        if (it == resources.end()) {
            LOG_ERROR(Lib_Videodec, "Resource id={} not found", resource_id);
            return is_be_wrapper ? ORBIS_VIDEO_CORE_ERROR_BE_WRAPPER_RESOURCE_NOT_FOUND
                                 : ORBIS_VIDEO_CORE_ERROR_RESOURCE_NOT_FOUND;
        }

        if (it->second.is_be_wrapper != is_be_wrapper) {
            LOG_ERROR(Lib_Videodec,
                      "Resource id={} type mismatch: requested is_be_wrapper={}, resource "
                      "is_be_wrapper={}",
                      resource_id, is_be_wrapper, it->second.is_be_wrapper);
            return is_be_wrapper ? ORBIS_VIDEO_CORE_ERROR_BE_WRAPPER_RESOURCE_NOT_FOUND
                                 : ORBIS_VIDEO_CORE_ERROR_RESOURCE_NOT_FOUND;
        }

        resources.erase(it);
        LOG_INFO(Lib_Videodec, "Released VideoCore resource id={}", resource_id);
        return ORBIS_OK;
    }

private:
    void EventThread(const std::stop_token& stop) {
        Common::SetCurrentThreadName("shadPS4:VideoCoreEvents");

        while (!stop.stop_requested()) {
            VideoCoreEvent event;
            {
                std::unique_lock lock(mutex);
                if (!event_cv.wait(lock, stop, [this] { return !events.empty(); })) {
                    break;
                }
                event = events.front();
                events.pop_front();
            }

            std::this_thread::sleep_until(event.deliver_at);

            {
                // The client may have released the resource while the event was in flight.
                std::unique_lock lock(mutex);
                if (!resources.contains(event.id)) {
                    LOG_DEBUG(Lib_Videodec, "Dropping event for released resource id={}", event.id);
                    continue;
                }
            }

            LOG_DEBUG(Lib_Videodec, "Notifying resource id={} status={}", event.id, event.status);
            event.callback(event.id, event.status);
        }
    }

    std::mutex mutex;
    u32 next_id{1};
    std::unordered_map<u32, VideoCoreResource> resources;
    std::deque<VideoCoreEvent> events;
    std::condition_variable_any event_cv;
    Kernel::Thread event_thread;
};

s32 PS4_SYSV_ABI
sceVideoCoreAcquireDecoderResource(const OrbisVideoCoreAcquireDecoderResourceParam* param,
                                   OrbisVideoCoreCallback cb, u32* out_resource_id) {
    return VideoCoreResourceManager::Instance().AcquireResource(param, cb, out_resource_id, false);
}

s32 PS4_SYSV_ABI sceVideoCoreReleaseDecoderResource(u32 resource_id) {
    return VideoCoreResourceManager::Instance().ReleaseResource(resource_id, false);
}

s32 PS4_SYSV_ABI sceVideoCoreAcquireDecoderResourceForBeWrapper(
    const OrbisVideoCoreAcquireDecoderResourceParam* param, OrbisVideoCoreCallback cb,
    u32* out_resource_id) {
    return VideoCoreResourceManager::Instance().AcquireResource(param, cb, out_resource_id, true);
}

s32 PS4_SYSV_ABI sceVideoCoreReleaseDecoderResourceForBeWrapper(u32 resource_id) {
    return VideoCoreResourceManager::Instance().ReleaseResource(resource_id, true);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("vDenqj6dzGk", "libSceVideoCoreServerInterface", 1,
                 "libSceVideoCoreServerInterface", sceVideoCoreAcquireDecoderResourceForBeWrapper);
    LIB_FUNCTION("dlG8cpQGq64", "libSceVideoCoreServerInterface", 1,
                 "libSceVideoCoreServerInterface", sceVideoCoreReleaseDecoderResourceForBeWrapper);
    LIB_FUNCTION("9eP06So7cfY", "libSceVideoCoreServerInterface", 1,
                 "libSceVideoCoreServerInterface", sceVideoCoreAcquireDecoderResource);
    LIB_FUNCTION("k2ZyhP7QdaA", "libSceVideoCoreServerInterface", 1,
                 "libSceVideoCoreServerInterface", sceVideoCoreReleaseDecoderResource);
}

} // namespace Libraries::VideoCoreServer
