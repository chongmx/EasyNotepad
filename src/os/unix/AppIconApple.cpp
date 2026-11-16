// macOS: the Dock's icon (os/Os.h: applyAppIcon). The app runs as a plain executable, not a
// bundle, so the Dock would show a generic one: the logo's tile (resources/icons) is set as the
// application's icon image once the window exists. AppKit through the Objective-C runtime (C++,
// no .mm): NSData and NSImage made with alloc/init and released here; NSApp keeps its own
// reference to the image, nothing waits on an autorelease pool.
#include "enp/os/Fs.h"
#include "enp/os/Log.h"
#include "enp/os/Os.h"

#include <objc/message.h>
#include <objc/runtime.h>

namespace enp::os {

namespace {

using Id = id;

Id send(Id target, const char* selector) { return reinterpret_cast<Id (*)(Id, SEL)>(objc_msgSend)(target, sel_registerName(selector)); }

Id cls(const char* name) { return reinterpret_cast<Id>(objc_getClass(name)); }

} // namespace

void applyAppIcon()
{
    const std::optional<std::string> png = fs::readAsset("icons/easynotepad-tile-512.png");
    if (!png || png->empty()) {
        ENP_LOG_WARN("the app's icon (icons/easynotepad-tile-512.png) is not among the resources");
        return;
    }
    Id data = reinterpret_cast<Id (*)(Id, SEL, const void*, unsigned long)>(objc_msgSend)(
        send(cls("NSData"), "alloc"), sel_registerName("initWithBytes:length:"), png->data(), static_cast<unsigned long>(png->size()));
    if (!data)
        return;
    Id image = reinterpret_cast<Id (*)(Id, SEL, Id)>(objc_msgSend)(send(cls("NSImage"), "alloc"), sel_registerName("initWithData:"), data);
    if (image) {
        Id app = send(cls("NSApplication"), "sharedApplication");
        reinterpret_cast<void (*)(Id, SEL, Id)>(objc_msgSend)(app, sel_registerName("setApplicationIconImage:"), image);
        send(image, "release");
    }
    send(data, "release");
}

} // namespace enp::os
