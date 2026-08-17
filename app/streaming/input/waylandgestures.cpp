#include "waylandgestures.h"

#ifdef HAS_WAYLAND

#include <SDL_syswm.h>
#include <wayland-client.h>
#include "wayland/pointer-gestures-unstable-v1-client-protocol.h"

#include <cstring>

// Version 1 is all we need: swipe and pinch begin/update/end. Version 2 adds
// hold gestures and 3 adds nothing we use, so asking for 1 keeps this working
// on the oldest compositor that has the protocol at all.
#define GESTURES_VERSION 1

const struct wl_registry_listener WaylandGestures::s_RegistryListener = {
    .global = WaylandGestures::registryGlobal,
    .global_remove = WaylandGestures::registryGlobalRemove,
};

const struct wl_seat_listener WaylandGestures::s_SeatListener = {
    .capabilities = WaylandGestures::seatCapabilities,
    .name = WaylandGestures::seatName,
};

const struct zwp_pointer_gesture_swipe_v1_listener WaylandGestures::s_SwipeListener = {
    .begin = WaylandGestures::swipeBegin,
    .update = WaylandGestures::swipeUpdate,
    .end = WaylandGestures::swipeEnd,
};

const struct zwp_pointer_gesture_pinch_v1_listener WaylandGestures::s_PinchListener = {
    .begin = WaylandGestures::pinchBegin,
    .update = WaylandGestures::pinchUpdate,
    .end = WaylandGestures::pinchEnd,
};

WaylandGestures::WaylandGestures()
    : m_Display(nullptr),
      m_Registry(nullptr),
      m_Seat(nullptr),
      m_Pointer(nullptr),
      m_Gestures(nullptr),
      m_Swipe(nullptr),
      m_Pinch(nullptr),
      m_SwipeFingers(0),
      m_SwipeDx(0),
      m_SwipeDy(0),
      m_PinchFingers(0),
      m_PinchScale(1.0),
      m_PinchRotation(0)
{
}

WaylandGestures::~WaylandGestures()
{
    if (m_Swipe != nullptr) {
        zwp_pointer_gesture_swipe_v1_destroy(m_Swipe);
    }
    if (m_Pinch != nullptr) {
        zwp_pointer_gesture_pinch_v1_destroy(m_Pinch);
    }
    if (m_Gestures != nullptr) {
        zwp_pointer_gestures_v1_destroy(m_Gestures);
    }
    if (m_Pointer != nullptr) {
        wl_pointer_destroy(m_Pointer);
    }
    if (m_Seat != nullptr) {
        wl_seat_destroy(m_Seat);
    }
    if (m_Registry != nullptr) {
        wl_registry_destroy(m_Registry);
    }

    // The destroys above are requests; without this they can outlive us.
    if (m_Display != nullptr) {
        wl_display_roundtrip(m_Display);
    }
}

bool WaylandGestures::initialize(SDL_Window* window)
{
    SDL_SysWMinfo info;

    SDL_VERSION(&info.version);

    if (!SDL_GetWindowWMInfo(window, &info)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_GetWindowWMInfo() failed: %s",
                     SDL_GetError());
        return false;
    }

    if (info.subsystem != SDL_SYSWM_WAYLAND) {
        // An X11 or KMSDRM session. Not a failure -- there is simply no
        // gesture protocol to bind.
        return false;
    }

    m_Display = info.info.wl.display;

    // Our own registry, and our own wl_seat binding with it. SDL keeps its
    // wl_pointer to itself, and a second pointer object on the same seat is
    // legitimate: the compositor delivers to every bound pointer whose
    // surface has focus, so SDL's input keeps working untouched.
    m_Registry = wl_display_get_registry(m_Display);
    if (m_Registry == nullptr) {
        return false;
    }
    wl_registry_add_listener(m_Registry, &s_RegistryListener, this);

    // Two round trips: the first delivers the globals, the second the seat
    // capabilities that say whether there is a pointer to hang gestures on.
    wl_display_roundtrip(m_Display);
    wl_display_roundtrip(m_Display);

    if (m_Gestures == nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Compositor does not offer zwp_pointer_gestures_v1, so "
                    "multi-finger touchpad gestures are unavailable");
        return false;
    }

    return m_Swipe != nullptr || m_Pinch != nullptr;
}

void WaylandGestures::registryGlobal(void* data, wl_registry* registry, uint32_t name,
                                     const char* interface, uint32_t version)
{
    auto me = (WaylandGestures*)data;

    if (std::strcmp(interface, "wl_seat") == 0 && me->m_Seat == nullptr) {
        // Cap at 2: that is the version whose events this listener covers.
        // Asking for more than we understand is how a client ends up being
        // sent an event it has no handler for.
        uint32_t bindVersion = version < 2 ? version : 2;
        me->m_Seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, bindVersion);
        if (me->m_Seat != nullptr) {
            wl_seat_add_listener(me->m_Seat, &s_SeatListener, me);
        }
    }
    else if (std::strcmp(interface, "zwp_pointer_gestures_v1") == 0 && me->m_Gestures == nullptr) {
        uint32_t bindVersion = version < GESTURES_VERSION ? version : GESTURES_VERSION;
        me->m_Gestures = (zwp_pointer_gestures_v1*)wl_registry_bind(
            registry, name, &zwp_pointer_gestures_v1_interface, bindVersion);

        // The seat's capabilities may already have arrived, in which case
        // this is the call that completes the pair.
        me->attachGestures();
    }
}

void WaylandGestures::registryGlobalRemove(void*, wl_registry*, uint32_t)
{
    // Nothing here can be hot-unplugged in a way we can act on: losing the
    // seat or the gesture manager mid-stream just stops the gestures.
}

void WaylandGestures::seatCapabilities(void* data, wl_seat*, uint32_t caps)
{
    auto me = (WaylandGestures*)data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && me->m_Pointer == nullptr) {
        me->m_Pointer = wl_seat_get_pointer(me->m_Seat);
        me->attachGestures();
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && me->m_Pointer != nullptr) {
        // The touchpad was unplugged. Drop the gesture objects with it --
        // they are attached to a pointer that no longer exists.
        if (me->m_Swipe != nullptr) {
            zwp_pointer_gesture_swipe_v1_destroy(me->m_Swipe);
            me->m_Swipe = nullptr;
        }
        if (me->m_Pinch != nullptr) {
            zwp_pointer_gesture_pinch_v1_destroy(me->m_Pinch);
            me->m_Pinch = nullptr;
        }
        wl_pointer_destroy(me->m_Pointer);
        me->m_Pointer = nullptr;
    }
}

void WaylandGestures::seatName(void*, wl_seat*, const char*)
{
}

void WaylandGestures::attachGestures()
{
    // Capabilities can arrive before the gesture manager does, depending on
    // the order the compositor advertises its globals, so this is called from
    // both sides and does nothing until both halves exist.
    if (m_Gestures == nullptr || m_Pointer == nullptr) {
        return;
    }

    if (m_Swipe == nullptr) {
        m_Swipe = zwp_pointer_gestures_v1_get_swipe_gesture(m_Gestures, m_Pointer);
        if (m_Swipe != nullptr) {
            zwp_pointer_gesture_swipe_v1_add_listener(m_Swipe, &s_SwipeListener, this);
        }
    }

    if (m_Pinch == nullptr) {
        m_Pinch = zwp_pointer_gestures_v1_get_pinch_gesture(m_Gestures, m_Pointer);
        if (m_Pinch != nullptr) {
            zwp_pointer_gesture_pinch_v1_add_listener(m_Pinch, &s_PinchListener, this);
        }
    }
}

void WaylandGestures::swipeBegin(void* data, zwp_pointer_gesture_swipe_v1*, uint32_t,
                                 uint32_t, wl_surface*, uint32_t fingers)
{
    auto me = (WaylandGestures*)data;

    me->m_SwipeFingers = fingers;
    me->m_SwipeDx = 0;
    me->m_SwipeDy = 0;
}

void WaylandGestures::swipeUpdate(void* data, zwp_pointer_gesture_swipe_v1*, uint32_t,
                                  wl_fixed_t dx, wl_fixed_t dy)
{
    auto me = (WaylandGestures*)data;

    me->m_SwipeDx += wl_fixed_to_double(dx);
    me->m_SwipeDy += wl_fixed_to_double(dy);
}

void WaylandGestures::swipeEnd(void* data, zwp_pointer_gesture_swipe_v1*, uint32_t,
                               uint32_t, int32_t cancelled)
{
    auto me = (WaylandGestures*)data;

    // A cancelled gesture is one libinput decided was something else after
    // all. Acting on it would fire on half the scrolls that start with a
    // stray third finger.
    if (!cancelled && me->m_SwipeFingers != 0 && me->onSwipe) {
        me->onSwipe(me->m_SwipeFingers, me->m_SwipeDx, me->m_SwipeDy);
    }

    me->m_SwipeFingers = 0;
}

void WaylandGestures::pinchBegin(void* data, zwp_pointer_gesture_pinch_v1*, uint32_t,
                                 uint32_t, wl_surface*, uint32_t fingers)
{
    auto me = (WaylandGestures*)data;

    me->m_PinchFingers = fingers;
    me->m_PinchScale = 1.0;
    me->m_PinchRotation = 0;
}

void WaylandGestures::pinchUpdate(void* data, zwp_pointer_gesture_pinch_v1*, uint32_t,
                                  wl_fixed_t, wl_fixed_t, wl_fixed_t scale, wl_fixed_t rotation)
{
    auto me = (WaylandGestures*)data;

    // scale is absolute, not a delta: the ratio against the finger distance
    // at begin. Keep the latest rather than accumulating.
    me->m_PinchScale = wl_fixed_to_double(scale);
    me->m_PinchRotation += wl_fixed_to_double(rotation);
}

void WaylandGestures::pinchEnd(void* data, zwp_pointer_gesture_pinch_v1*, uint32_t,
                               uint32_t, int32_t cancelled)
{
    auto me = (WaylandGestures*)data;

    if (!cancelled && me->m_PinchFingers != 0 && me->onPinch) {
        me->onPinch(me->m_PinchFingers, me->m_PinchScale, me->m_PinchRotation);
    }

    me->m_PinchFingers = 0;
}

#endif
