#pragma once

#include "Downloader.hpp"
#include <coco/UsbDevice.hpp>


namespace coco {

constexpr int CONTROL_BUFFER_SIZE = 512;

/// @brief Handle SCPI commands
/// @param loop Event loop
/// @param jusb USB device
/// @param buffer USB control buffer (size is 512)
/// @param downloader Downloader (flash info section must be in downloader.flashInfos[0])
coco::Coroutine handleDfu(Loop &loop, coco::UsbDevice &usb, Buffer &buffer,
    Downloader &downloader,
    uint16_t vid, uint16_t pid, uint16_t version);

} // namespace coco
