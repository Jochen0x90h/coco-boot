#pragma once

#include <coco/Array.hpp>
#include <coco/Barrier.hpp>
#include <coco/BufferReader.hpp>
#include <coco/BufferWriter.hpp>
#include <coco/convert.hpp>
#include <coco/debug.hpp>
#include <coco/Coroutine.hpp>
#include <coco/Loop.hpp>
#include <coco/platform/flash.hpp>
#include <coco/platform/system.hpp>


namespace coco {

using namespace coco::literals;

constexpr int BOOT_RESET_INTENT = 1;
constexpr int APP_RESET_INTENT = 0;


/// @brief Downloader
///
struct Downloader {
    // firmware (.fw) file format
    static constexpr int HEADER_SIZE = 24;
    static constexpr int SECTION_HEADER_SIZE = 12;

    // section IDs
    static constexpr int APP_ID = 1;
    static constexpr int STORAGE_ID = 2;
    static constexpr int INFO_ID = 8000; // device info
    static constexpr int BOOTLOADER_ID = 8001;

    // section flags
    enum class Flags : uint16_t {
        NONE = 0,
        APPLICATION = 1 << 0, // section is an application (has vector table)
    };

    // application
    static constexpr int APP_INFO_OFFSET = 7; // offset of size and crc in the vector table (in 32 bit words)

    // flash section info (bootloader, device info, application, config storage)
    struct FlashSection {
        uint16_t id;
        Flags flags;
        uint32_t address;
        int minSize;
        int maxSize;
    };

    // download state
    enum class State {
        // wait for data
        WAIT,

        // busy with some transfer task
        BUSY,

        // download finished
        FINISHED,

        // error
        ERROR
    };

    // configuration
    String firmwareId;
    coco::Array<const FlashSection> flashSections;
    union {
        uint8_t *u8;
        uint32_t *u32;
        coco::flash::Word *blocks;
    } buffer;
    const int bufferSize;

    // state
    bool enableJump = true;
    State state = State::BUSY;
    int bufferPos = 0;
    coco::Barrier<> bufferBarrier;
    int totalSize = 0;


    /// @brief Constructor.
    /// @param firmwareId Firware ID that must be in the header of firmware update files (.fw)
    /// @param flashInfos Array of flash section infos (array size 4: BOOTLOADER, INFO, APP, STORAGE)
    /// @param buffer Buffer memory
    /// @param bufferSize Size of buffer (if bootloader self-update is used, it must be equal to the B template parameter of download())
    Downloader(String firmwareId, coco::Array<const FlashSection> flashSections, uint8_t *buffer, int bufferSize)
        : firmwareId(firmwareId), flashSections(flashSections), buffer(buffer), bufferSize(bufferSize) {}

    /// @brief Get the firmware CRC.
    /// The CRC is stored in the vector table of the application firmware (APP_INFO_OFFSET + 1).
    /// @param index Index of the flash section (must be an application section)
    /// @return The firmware CRC
    uint32_t getFirmwareCrc(int index) {
        auto app = reinterpret_cast<const uint32_t *>(flashSections[index].address);
        return app[APP_INFO_OFFSET + 1];
    }

    /// @brief Check the firmware.
    /// @tparam C Crc calculator type
    /// @param crc CRC instance to use for checking the firmware
    /// @return true if the firmware is valid, false otherwise
    template <typename C>
    bool checkFirmware(int index, C &crc) {
        auto data = reinterpret_cast<const uint32_t *>(flashSections[index].address);
        static const uint32_t zeros[4] = {0, 0, 0, 0};

        // get size of crc from unused vector table entries
        auto size = data[APP_INFO_OFFSET + 0];
        auto storedCrc = data[APP_INFO_OFFSET + 1];

        // check inverted size and crc
        if (size != ~data[APP_INFO_OFFSET + 2])
            return false;
        if (storedCrc != ~data[APP_INFO_OFFSET + 3])
            return false;

        uint32_t currentCrc = 0xffffffff;

        // calc crc first 7 words
        currentCrc = crc.calc(currentCrc, data, APP_INFO_OFFSET);

        // insert 4 words of zeros for the unused vector table entries
        currentCrc = crc.calc(currentCrc, zeros);

        // calc crc of remaining data
        currentCrc = crc.calc(currentCrc, data + APP_INFO_OFFSET + 4, ((size + 3) >> 2) - (APP_INFO_OFFSET + 4));

        return storedCrc == currentCrc;
    }

    /// @brief Copy boot loader from buffer into flash
    /// Uses FLASH_BASE as the destination address, which is the start of the flash memory.
    /// This function does not return, it jumps into the new bootloader.
    /// The function is executed in CCMRAM to be independent of flash access.
    /// The called functions therefore need to be inlined.
    __attribute__((noreturn)) __attribute__((section(".ccmram")))
    void copyBootloader() {
        // disable interrupts
        __disable_irq();

        // erase bootloader
        coco::flash::erase(FLASH_BASE, bufferPos);

        // write new bootloader
        coco::flash::write(FLASH_BASE, buffer.blocks, bufferPos);

        // restart and boot into application
        coco::system::reset(APP_RESET_INTENT);
    }

    /// @brief Download firmware into device (flash, Nextion display, etc.).
    /// After the coroutine has been started (e.g. in main()), it waits on the bufferBarrier for data. Bootloader frontends
    /// such as ScpiBoot or DfuBoot write data into the buffer and call bufferBarrier.doFirst() to notify the downloader.
    /// @tparam B Maximum size of bootloader section (BOOTLOADER_ID), 0 to disable self-update of bootloader
    /// @tparam C Crc calculator type
    /// @param loop Event loop
    /// @param firmwareId Firmware ID to check in the file header
    /// @param crc CRC calculator
    /// @param flashInfos Flash info array: boot loader, device info, application, storage (in this order)
    template <int B, typename C, typename... Args>
    coco::Coroutine download(Loop &loop, C &crc, Args &... args) {
        while (true)  {
            // wait until a complete file header is in the buffer
            state = State::WAIT;
            while (bufferPos < HEADER_SIZE)
                co_await bufferBarrier.untilResumed();

            // read header
            //debug::out << "read header\n";
            BufferReader r(buffer.u8, bufferPos);
            String firmwareId2 = r.string(16); // string references the buffer
            r.u32L(); // flags
            totalSize = r.u32L(); // does not include the header

            // check header
            if (firmwareId == firmwareId2 && totalSize >= HEADER_SIZE && totalSize <= 134217728) {
                // remove file header from buffer
                std::copy(buffer.u8 + HEADER_SIZE, buffer.u8 + bufferPos, buffer.u8);
                bufferPos -= HEADER_SIZE;
                //debug::out << "header OK\n";

                // iterate over all sections
                while (totalSize >= SECTION_HEADER_SIZE) {
                    // wait until a complete section header is in the buffer
                    state = State::WAIT;
                    while (bufferPos < SECTION_HEADER_SIZE)
                        co_await bufferBarrier.untilResumed();

                    // read section header
                    BufferReader r(buffer.u8, bufferPos);
                    int sectionId = r.u32L();
                    int sectionCrc = r.u32L();
                    int currentCrc = 0xffffffff;
                    int sectionSize = r.u32L();

                    // remove section header from buffer
                    std::copy(buffer.u8 + SECTION_HEADER_SIZE, buffer.u8 + bufferPos, buffer.u8);
                    bufferPos -= SECTION_HEADER_SIZE;
                    totalSize -= SECTION_HEADER_SIZE;

                    switch (sectionId) {
                    case BOOTLOADER_ID:
                        if constexpr (B > 0) {
                            // get flash info (BOOTLOADER: index 0)
                            //auto &info = flashInfos[0];
                            if (sectionSize >= 1024 && sectionSize <= B) {
                                // wait until whole bootloader is in the buffer
                                state = State::WAIT;
                                int toUpload = sectionSize;
                                while (bufferPos < toUpload)
                                    co_await bufferBarrier.untilResumed();

                                // update crc
                                int size = (toUpload + 3) >> 2;
                                std::fill(buffer.u8 + toUpload, buffer.u8 + size * 4, 0); // zero pad for crc
                                currentCrc = crc.calc(currentCrc, buffer.u32, size);

                                if (currentCrc == sectionCrc) {
                                    // copy bootloader, does not return
                                    copyBootloader();
                                }
                            }
                        }
                        break;
                    default:
                        // search for flash section with matching ID
                        for (auto &info : flashSections) {
                            if (info.id == sectionId) {
                                uint32_t flashAddress = info.address;
                                if (sectionSize == 0) {
                                    // section size 0 means erase entire flash section
                                    coco::flash::erase(flashAddress, info.maxSize);
                                } else if (sectionSize >= info.minSize && sectionSize <= info.maxSize) {
                                    while (sectionSize > 0) {
                                        // wait until buffer is full or section at end
                                        state = State::WAIT;
                                        int toUpload = std::min(coco::flash::PAGE_SIZE, sectionSize);
                                        while (bufferPos < toUpload)
                                            co_await bufferBarrier.untilResumed();

                                        // update crc
                                        int size = (toUpload + 3) >> 2;
                                        std::fill(buffer.u8 + toUpload, buffer.u8 + size * 4, 0); // zero pad for crc
                                        currentCrc = crc.calc(currentCrc, buffer.u32, size);

                                        // insert size and crc of application into unused elements of the vector table
                                        if ((int(info.flags) & int(Downloader::Flags::APPLICATION)) != 0 && flashAddress == info.address) {
                                            buffer.u32[APP_INFO_OFFSET + 0] = sectionSize;
                                            buffer.u32[APP_INFO_OFFSET + 1] = sectionCrc;
                                            buffer.u32[APP_INFO_OFFSET + 2] = ~sectionSize;
                                            buffer.u32[APP_INFO_OFFSET + 3] = ~sectionCrc;
                                        }

                                        // write to flash
                                        coco::flash::erase(flashAddress, toUpload);
                                        coco::flash::write(flashAddress, buffer.blocks, toUpload);
                                        flashAddress += toUpload;

                                        // remove uploaded part from buffer
                                        std::copy(buffer.u8 + toUpload, buffer.u8 + bufferPos, buffer.u8);
                                        bufferPos -= toUpload;
                                        totalSize -= toUpload;
                                        sectionSize -= toUpload;
                                    }
                                }
                            }
                        }
                    }

                    // section is finished
                    if (sectionSize > 0 || currentCrc != sectionCrc) {
                        // error: section not finished or CRC mismatch
                        state = State::ERROR;
                        break;
                    }
                } // sections
            }

            // download is finished
            if (totalSize == 0 && bufferPos == 0 && state != State::ERROR) {
                // success
                //debug::set(debug::GREEN);
                state = State::FINISHED;
                //debug::out << "FINISHED\n";
            } else {
                // error
                debug::set(debug::RED);
                state = State::ERROR;
                //debug::out << "ERROR\n";
            }

            // stay in finished or error state until resumed in BUSY state to restart
            while (state != State::BUSY)
                co_await bufferBarrier.untilResumed();
            bufferPos = 0;
            totalSize = 0;
        } // main loop
    }
};
COCO_ENUM(Downloader::Flags)

} // namespace coco
