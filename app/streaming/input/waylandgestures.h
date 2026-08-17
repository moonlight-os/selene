#pragma once

#include <SDL.h>

#ifdef HAS_WAYLAND

#include <functional>

// For wl_fixed_t, which the gesture callbacks below take. This is the small
// header of the pair -- it declares no protocol objects, so the interfaces
// stay forward-declared and this does not pull wayland-client.h into every
// translation unit that includes the input handler.
#include <wayland-util.h>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct wl_pointer;
struct wl_surface;
struct zwp_pointer_gestures_v1;
struct zwp_pointer_gesture_swipe_v1;
struct zwp_pointer_gesture_pinch_v1;

// Multi-finger touchpad gestures, which SDL does not deliver on Wayland.
//
// A touchpad is not a touch device as far as the compositor is concerned:
// libinput consumes its touches and hands clients pointer motion, scroll and
// -- only through zwp_pointer_gestures_v1 -- whole gestures. SDL's Wayland
// backend never binds that protocol, so the client's own SDL_FINGER path
// (MAX_FINGERS, which is 2 anyway) sees nothing from a touchpad at all.
//
// Binding it ourselves is the whole reason for this class. It piggybacks on
// SDL's display and its event pump exactly as WaylandVsyncSource does, so
// there is no second event queue and no extra thread.
//
// Measured ceiling, on real hardware rather than from the specification:
// libinput reports 3- and 4-finger swipes and 2-finger pinches. It does not
// report 5-finger gestures even when the hardware has five slots -- those
// arrive as 4-finger ones. Do not promise five.
class WaylandGestures
{
public:
    WaylandGestures();
    ~WaylandGestures();

    // False when the window is not a Wayland one, or the compositor does not
    // offer the protocol. Both are ordinary outcomes, not errors.
    bool initialize(SDL_Window* window);

    // dx/dy are the accumulated movement over the whole gesture, in the
    // compositor's coordinate space. scale is the pinch factor at the end,
    // where 1.0 is unchanged.
    std::function<void(uint32_t fingers, double dx, double dy)> onSwipe;
    std::function<void(uint32_t fingers, double scale, double rotation)> onPinch;

private:
    // Members rather than file-scope tables: their initialisers then sit in
    // class scope and can name the private callbacks below, which is the same
    // arrangement WaylandVsyncSource uses.
    static const struct wl_registry_listener s_RegistryListener;
    static const struct wl_seat_listener s_SeatListener;
    static const struct zwp_pointer_gesture_swipe_v1_listener s_SwipeListener;
    static const struct zwp_pointer_gesture_pinch_v1_listener s_PinchListener;

    static void registryGlobal(void* data, wl_registry* registry, uint32_t name,
                               const char* interface, uint32_t version);
    static void registryGlobalRemove(void* data, wl_registry* registry, uint32_t name);

    static void seatCapabilities(void* data, wl_seat* seat, uint32_t caps);
    static void seatName(void* data, wl_seat* seat, const char* name);

    static void swipeBegin(void* data, zwp_pointer_gesture_swipe_v1* g, uint32_t serial,
                           uint32_t time, wl_surface* surface, uint32_t fingers);
    static void swipeUpdate(void* data, zwp_pointer_gesture_swipe_v1* g, uint32_t time,
                            wl_fixed_t dx, wl_fixed_t dy);
    static void swipeEnd(void* data, zwp_pointer_gesture_swipe_v1* g, uint32_t serial,
                         uint32_t time, int32_t cancelled);

    static void pinchBegin(void* data, zwp_pointer_gesture_pinch_v1* g, uint32_t serial,
                           uint32_t time, wl_surface* surface, uint32_t fingers);
    static void pinchUpdate(void* data, zwp_pointer_gesture_pinch_v1* g, uint32_t time,
                            wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation);
    static void pinchEnd(void* data, zwp_pointer_gesture_pinch_v1* g, uint32_t serial,
                         uint32_t time, int32_t cancelled);

    void attachGestures();

    wl_display* m_Display;
    wl_registry* m_Registry;
    wl_seat* m_Seat;
    wl_pointer* m_Pointer;
    zwp_pointer_gestures_v1* m_Gestures;
    zwp_pointer_gesture_swipe_v1* m_Swipe;
    zwp_pointer_gesture_pinch_v1* m_Pinch;

    uint32_t m_SwipeFingers;
    double m_SwipeDx;
    double m_SwipeDy;

    uint32_t m_PinchFingers;
    double m_PinchScale;
    double m_PinchRotation;
};

#endif
