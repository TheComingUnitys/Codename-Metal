#!/usr/bin/env python3
"""CNE Metal: keep the game updating while AppKit tracks the window menu.

Hovering the green traffic-light button makes AppKit enter an event-tracking run loop; SDL's
blocking SDL_WaitEvent() does not return until the menu closes, so the picture freezes while
the audio thread keeps playing.  This patch adds a public SDLApplication entry point that
dispatches one UPDATE + RENDER directly (without touching the SDL event queue, which is what
caused the earlier re-entrancy crash).

Idempotent via the CNE_METAL_MENU_PUMP marker.
"""
import argparse
import os
import sys

MARK = "CNE_METAL_MENU_PUMP"

HEADER_ANCHOR = "\t\t\tvoid RegisterWindow (SDLWindow *window);"
HEADER_INSERT = (
    "\t\t\t// " + MARK + ": dispatch UPDATE+RENDER while AppKit tracks a window menu\n"
    "\t\t\t// (green zoom button hover). Keeps the picture moving instead of freezing.\n"
    "\t\t\tstatic void PumpDuringMenuTracking ();\n\n"
    + HEADER_ANCHOR
)

CPP_ANCHOR = "\tvoid SDLApplication::UpdateFrame () {"
CPP_INSERT = """\tvoid SDLApplication::PumpDuringMenuTracking () {

\t\t// """ + MARK + """
\t\tif (currentApplication == nullptr) return;

\t\tSDLApplication* app = currentApplication;
\t\tif (!app->active) return;

\t\t// Dispatch one update+render directly.  No SDL_PeepEvents here: calling back into
\t\t// SDL's event pump from inside AppKit's nested tracking loop re-entered the queue and
\t\t// crashed (EXC_BREAKPOINT on the main thread).
\t\t// WaitEvent() on macOS has already entered System::GCEnterBlocking() before its
\t\t// SDL_WaitEvent(); dispatching Haxe code inside that blocking zone causes
\t\t// "Critical Error: GCFree Zone mismatch".  Leave/re-enter the blocking zone around
\t\t// our dispatch.
\t\tUint32 now = SDL_GetTicks ();
\t\tapp->applicationEvent.type = UPDATE;
\t\tapp->applicationEvent.deltaTime = now - app->lastUpdate;
\t\tapp->lastUpdate = now;

\t\tSystem::GCExitBlocking ();
\t\tApplicationEvent::Dispatch (&app->applicationEvent);
\t\tRenderEvent::Dispatch (&app->renderEvent);
\t\tSystem::GCEnterBlocking ();
\t}


""" + CPP_ANCHOR


def patch_file(path, anchor, insert, label):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if MARK in text:
        return False
    if anchor not in text:
        raise SystemExit("error: %s: anchor not found (%s)" % (path, label))
    text = text.replace(anchor, insert, 1)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lime", required=True)
    args = parser.parse_args()
    header = os.path.join(args.lime, "project/src/backend/sdl/SDLApplication.h")
    cpp = os.path.join(args.lime, "project/src/backend/sdl/SDLApplication.cpp")
    changed = patch_file(header, HEADER_ANCHOR, HEADER_INSERT, "SDLApplication.h")
    changed = patch_file(cpp, CPP_ANCHOR, CPP_INSERT, "SDLApplication.cpp") or changed
    print("[patch-menu-pump] %s" % ("applied" if changed else "already applied"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
