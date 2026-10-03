#include "wheelio_bridge_c.h"

#include "bridge_server.hpp"
#include "vr_frame_reader.hpp"

#include <IOKit/hidsystem/IOHIDLib.h>

#include <cstring>
#include <new>

// WheelioBridge owns the C++ engine. BridgeServer holds a std::thread/std::mutex so
// it is neither copyable nor movable; constructing it in place avoids any move.
struct WheelioBridge {
    BridgeServer server;
    // Independent of the wheel: reads frames the OpenXR runtime publishes.
    VRFrameReader vr_reader;
    explicit WheelioBridge(std::uint16_t port) : server(port) {}
};

namespace {

// Copy a std::string into a fixed C buffer, always NUL-terminated. Same pattern
// the engine already uses for HelloAckPayload (bridge_server.cpp).
void copy_field(char *dst, std::size_t dst_size, const std::string &src) {
    if (dst_size == 0) {
        return;
    }
    std::strncpy(dst, src.c_str(), dst_size - 1);
    dst[dst_size - 1] = '\0';
}

}  // namespace

WheelioBridge *wheelio_bridge_create(uint16_t port) {
    const std::uint16_t effective_port = port != 0 ? port : wheelio_bridge::kDefaultPort;
    return new (std::nothrow) WheelioBridge(effective_port);
}

void wheelio_bridge_start(WheelioBridge *bridge) {
    if (bridge != nullptr) {
        bridge->server.start();
    }
}

void wheelio_bridge_stop(WheelioBridge *bridge) {
    if (bridge != nullptr) {
        bridge->server.stop();
    }
}

void wheelio_bridge_reconnect_wheel(WheelioBridge *bridge) {
    if (bridge != nullptr) {
        bridge->server.reconnect_wheel();
    }
}

void wheelio_bridge_set_force_gain(WheelioBridge *bridge, double gain) {
    if (bridge != nullptr) {
        bridge->server.set_force_gain(gain);
    }
}

void wheelio_bridge_set_spring_gain(WheelioBridge *bridge, double gain) {
    if (bridge != nullptr) {
        bridge->server.set_spring_gain(gain);
    }
}

void wheelio_bridge_set_damper_gain(WheelioBridge *bridge, double gain) {
    if (bridge != nullptr) {
        bridge->server.set_damper_gain(gain);
    }
}

void wheelio_bridge_set_smoothing(WheelioBridge *bridge, double amount) {
    if (bridge != nullptr) {
        bridge->server.set_smoothing(amount);
    }
}

void wheelio_bridge_set_min_force(WheelioBridge *bridge, double fraction) {
    if (bridge != nullptr) {
        bridge->server.set_min_force(fraction);
    }
}

void wheelio_bridge_self_test(WheelioBridge *bridge) {
    if (bridge != nullptr) {
        bridge->server.run_self_test();
    }
}

void wheelio_bridge_status(WheelioBridge *bridge, WheelioBridgeStatus *out) {
    if (bridge == nullptr || out == nullptr) {
        return;
    }

    const BridgeServer::Status status = bridge->server.status();
    out->listening = status.listening;
    out->client_connected = status.client_connected;
    out->wheel_connected = status.wheel_connected;
    out->port = status.port;
    out->packets_received = status.packets_received;
    copy_field(out->client_name, sizeof(out->client_name), status.client_name);
    copy_field(out->wheel_name, sizeof(out->wheel_name), status.wheel_name);
}

void wheelio_bridge_destroy(WheelioBridge *bridge) {
    delete bridge;
}

WheelioInputMonitoringState wheelio_input_monitoring_status(void) {
    switch (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)) {
        case kIOHIDAccessTypeGranted:
            return WheelioInputMonitoringGranted;
        case kIOHIDAccessTypeDenied:
            return WheelioInputMonitoringDenied;
        case kIOHIDAccessTypeUnknown:
        default:
            return WheelioInputMonitoringUnknown;
    }
}

bool wheelio_input_monitoring_request(void) {
    return IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
}

void wheelio_vr_poll(WheelioBridge *bridge) {
    if (bridge != nullptr) {
        bridge->vr_reader.poll();
    }
}

void wheelio_vr_status(WheelioBridge *bridge, WheelioVRStatus *out) {
    if (out == nullptr) {
        return;
    }
    *out = WheelioVRStatus{};
    if (bridge == nullptr) {
        return;
    }
    const VRFrameReader::Status status = bridge->vr_reader.status();
    out->connected = status.connected;
    out->game_running = status.game_running;
}
