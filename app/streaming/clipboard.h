#pragma once

#include <QString>

// Setting the clipboard, which is not as simple as SDL makes it look.
//
// SDL_SetClipboardText() returns success on Wayland without the compositor
// ever taking the selection. Measured on Moonlight OS: the client received the
// text, SDL reported the set succeeded and logged "Detected Wayland", and
// wl-paste still reported an empty clipboard -- with no competing selection
// owner at all, and with the window focused. A silent failure is worse than a
// loud one, because nothing downstream can detect it and fall back.
//
// So on Linux the session's own clipboard tool is used, exactly as Helios does
// on the host side and for the same reason: a selection lives in the
// compositor, owned by a client that must be able to answer for it. Everywhere
// else SDL is fine and is used unchanged.
//
// The eventual right answer on Wayland is for Selene to own the selection
// itself through wl_data_device -- it already binds its own wl_seat for
// touchpad gestures, so the scaffolding exists. That is a larger change than
// this, and this makes the feature work today.
namespace Clipboard
{
    // True when the text reached the platform clipboard. Unlike SDL's return
    // value, this one means it.
    bool setText(const QString& text);
}
