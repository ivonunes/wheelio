#pragma once

#include "wheelio_bridge_protocol.hpp"

namespace wheelio_bridge {

// The force-feedback state the game last asked for, and whether the other end
// still needs it. Every state message is the complete desired state, so after
// a failed delivery or a receiver that lost it (the proxy reconnecting, the
// Mac reopening the wheel) delivering the last one again restores everything.
// Without this, a centering spring the game set once stayed lost until the
// game happened to change it.
class DesiredWheelState {
public:
    // A new state from the game: it needs delivering.
    void set(const WheelStatePayload& state) {
        state_ = state;
        have_state_ = true;
        pending_ = true;
    }

    // The game stopped all forces: nothing left to deliver.
    void clear() {
        have_state_ = false;
        pending_ = false;
    }

    void delivered() { pending_ = false; }

    // The delivery failed or the receiver lost what it had: deliver again.
    void redeliver() { pending_ = have_state_; }

    bool needs_delivery() const { return have_state_ && pending_; }
    bool has_state() const { return have_state_; }
    const WheelStatePayload& state() const { return state_; }

private:
    WheelStatePayload state_{};
    bool have_state_ = false;
    bool pending_ = false;
};

}  // namespace wheelio_bridge
