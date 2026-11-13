#include "DfuBoot.hpp"
#include <coco/usb.hpp>
#include <coco/platform/system.hpp>


namespace coco {
using coco::UsbDevice;
namespace usb = coco::usb;
namespace dfu = coco::usb::dfu;
namespace msos20 = coco::usb::msos20;


// string id
enum class StringId : uint8_t {
    LANGUAGES = 0,
    MANUFACTURER = 1,
    PRODUCT = 2,
    SERIAL = 3,
    DFU_INTERFACE = 4,
};

// vendor specific control request
enum class VendorRequest : uint8_t {
    WINUSB = 1,
};


// configuration descriptor
struct UsbConfiguration {
    usb::ConfigurationDescriptor config;
    usb::InterfaceDescriptor dfuInterface;
    dfu::FunctionalDescriptor dfuDescriptor;
};
static_assert(sizeof(UsbConfiguration) <= 256);

static const UsbConfiguration configurationDescriptor = {
    .config = {
        .wTotalLength = sizeof(UsbConfiguration),
        .bNumInterfaces = 1,
        .bConfigurationValue = 1,
        .iConfiguration = 0,
        .bmAttributes = usb::ConfigurationDescriptorAttriubtes::BUS_POWERED,
        .bMaxPower = 50 // 100 mA
    },
    .dfuInterface = {
        .bInterfaceNumber = 0,
        .bAlternateSetting = 0,
        .bNumEndpoints = 0,
        .bInterfaceClass = usb::InterfaceClass::APPLICATION,
        .bInterfaceSubClass = usb::InterfaceSubClass::DFU,
        .bInterfaceProtocol = usb::InterfaceProtocol::DFU_BOOT,
        .iInterface = uint8_t(StringId::DFU_INTERFACE) // index into string table
    },
    .dfuDescriptor = {
        .bmAttributes = dfu::Attributes::CAN_DNLOAD | dfu::Attributes::MANIFESTATION_TOLERANT,
        .wDetachTimeout = 100,
        .wTransferSize = CONTROL_BUFFER_SIZE
    }
};


// string descriptors
const usb::StringDescriptor<1> languages = {
    .wString = {0x0409} // English (United States)
};
const auto manufacturerString = usb::makeStringDescriptor(u"Fritz Kübler GmbH");
const auto productString = usb::makeStringDescriptor(u"EP1000 Boot");
const auto serialString = usb::makeStringDescriptor(u"12345");
const auto dfuInterfaceString = usb::makeStringDescriptor(u"EP1000 Boot DFU");


// binary object store (BOS) descriptor with MS OS 2.0 platform capability descriptor
struct Bos {
    usb::BosDescriptor bos;
    msos20::PlatformCapabilityDescriptorHeader header;
    msos20::DescriptorSetInformation info;
};
static_assert(sizeof(Bos) <= CONTROL_BUFFER_SIZE);

// descriptor that instructs Windows to use WinUSB driver
// Creates registry entry: HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\USB\VID_1209&PID_0001\12345
// Example: https://github.com/pololu/libusbp/blob/master/test/firmware/wixel/main.c
struct WinUsbDescriptor {
    msos20::DescriptorSetHeader header;
    msos20::CompatibleIdDescriptor compatibleId;
    msos20::RegistryPropertyDescriptor<21, 40> registryProperty; // length of PropertyName and PropertyData
};
static_assert(sizeof(WinUsbDescriptor) <= CONTROL_BUFFER_SIZE);

static const Bos bos {
    .bos = {
        .wTotalLength = sizeof(Bos),
        .bNumDeviceCaps = 1
    },
    .header = {
        .bLength = sizeof(Bos) - sizeof(usb::BosDescriptor),
    },
    .info = {
        .wMSOSDescriptorSetTotalLength = sizeof(WinUsbDescriptor),
        .bMS_VendorCode = uint8_t(VendorRequest::WINUSB)
    }
};

static const WinUsbDescriptor winUsbDescriptor {
    .header = {
        .wTotalLength = sizeof(WinUsbDescriptor)
    },
    .compatibleId = {
        .CompatibleID = "WINUSB"
    },
    .registryProperty = {
        .wPropertyDataType = msos20::PropertyDatatype::MULTI_SZ,
        .PropertyName = u"DeviceInterfaceGUIDs",
        .PropertyData = u"{f61344d8-ef1d-43ee-887c-bba72018facf}\0"
    }
};


// handle control requests
Coroutine handleDfu(Loop &loop, UsbDevice &usb, Buffer &buffer,
    Downloader &downloader,
    uint16_t vid, uint16_t pid, uint16_t version)
{
    dfu::State dfuState = dfu::State::DFU_IDLE;
    while (true) {
        // wait for a control request (https://www.beyondlogic.org/usbnutshell/usb6.shtml)
        co_await usb.untilRequest();
        auto setup = usb.getSetup();

        // handle request
        switch (setup.bmRequestType) {
        case usb::RequestType::STANDARD_DEVICE_IN:
            switch (setup.bRequest) {
            case usb::Request::GET_DESCRIPTOR:
                {
                    auto descriptorType = usb::DescriptorType(setup.wValue >> 8);
                    switch (descriptorType) {
                    case usb::DescriptorType::DEVICE:
                        // send device descriptor to host
                        //co_await UsbDevice::controlIn(buffer, setup, deviceDescriptor);

                        reinterpret_cast<usb::DeviceDescriptor &>(*buffer.data()) = {
                            .bcdUSB = 0x0210, // USB version 2.10 to support BOS descriptor
                            .bDeviceClass = usb::DeviceClass::NONE,
                            .bDeviceSubClass = usb::DeviceSubClass::NONE,
                            .bDeviceProtocol = 0,
                            .bMaxPacketSize0 = 64, // max packet size for endpoint 0
                            .idVendor = vid,
                            .idProduct = pid,
                            .bcdDevice = version, // device version
                            .iManufacturer = uint8_t(StringId::MANUFACTURER), // index into string table
                            .iProduct = uint8_t(StringId::PRODUCT), // index into string table
                            .iSerialNumber = uint8_t(StringId::SERIAL), // index into string table
                            .bNumConfigurations = 1
                        };
                        co_await buffer.write(std::min(int(setup.wLength), int(sizeof(usb::DeviceDescriptor))));

                        break;
                    case usb::DescriptorType::CONFIGURATION:
                        // send configuration descriptor to host
                        co_await UsbDevice::controlIn(buffer, setup, configurationDescriptor);
                        break;
                    case usb::DescriptorType::STRING:
                        switch (StringId(setup.wValue & 0xff)) {
                        case StringId::LANGUAGES:
                            co_await UsbDevice::controlIn(buffer, setup, languages);
                            break;
                        case StringId::MANUFACTURER:
                            co_await UsbDevice::controlIn(buffer, setup, manufacturerString);
                            break;
                        case StringId::PRODUCT:
                            co_await UsbDevice::controlIn(buffer, setup, productString);
                            break;
                        case StringId::SERIAL:
                            // todo: use serial number from downloader.flashInfos
                            co_await UsbDevice::controlIn(buffer, setup, serialString);
                            break;
                        case StringId::DFU_INTERFACE:
                            co_await UsbDevice::controlIn(buffer, setup, dfuInterfaceString);
                            break;
                        default:
                            usb.stall();
                        }
                        break;
                    case usb::DescriptorType::BOS:
                        // send BOS descriptor to host
                        co_await UsbDevice::controlIn(buffer, setup, bos);
                        break;
                    default:
                        usb.stall();
                    }
                }
                break;
            default:
                usb.stall();
            }
            break;
        case usb::RequestType::CLASS_INTERFACE_IN:
            switch (setup.bRequest) {
            case dfu::Request::DFU_UPLOAD:
                //debug::out << "DFU_UPLOAD wValue " << dec(setup.wValue) << " wLength " << dec(setup.wLength) << '\n';

                // not supported
                usb.stall();
                break;
            case dfu::Request::DFU_GETSTATE:
                buffer.cast<dfu::State &>() = dfuState;
                co_await buffer.write(std::min(int(setup.wLength), 1));
                break;
            case dfu::Request::DFU_GETSTATUS:
                //debug::out << "DFU_GETSTATUS wValue " << dec(setup.wValue) << " wLength " << dec(setup.wLength) << '\n';
                {
                    switch (dfuState) {
                    case dfu::State::DFU_IDLE:
                        // start: disable jump to application
                        downloader.enableJump = false;
                        break;
                    case dfu::State::DFU_DNLOAD_SYNC:
                        if (downloader.state == Downloader::State::ERROR) {
                            dfuState = dfu::State::DFU_ERROR;
                        } else if (downloader.bufferPos + (downloader.state == Downloader::State::BUSY ? CONTROL_BUFFER_SIZE : 0) <= downloader.bufferSize) {
                            dfuState = dfu::State::DFU_DNLOAD_IDLE;
                            //debug::set(debug::GREEN);
                        }
                        break;
                    case dfu::State::DFU_MANIFEST_SYNC:
                        if (downloader.state == Downloader::State::WAIT || downloader.state == Downloader::State::ERROR) {
                            // error: download is still waiting for data or has encountered an error
                            dfuState = dfu::State::DFU_ERROR;
                        } else if (downloader.state == Downloader::State::FINISHED) {
                            // finished: go to idle state
                            dfuState = dfu::State::DFU_IDLE;
                        } // else stay in sync as long as download coroutine is busy
                        break;
                    case dfu::State::DFU_MANIFEST:
                        dfuState = dfu::State::DFU_MANIFEST_SYNC; // manifestation tolerant bit set
                        break;
                    default:
                        ;
                    }

                    auto &status = buffer.cast<dfu::StatusReport &>();
                    status = {
                        dfu::Status::OK,
                        200, // 200ms
                        dfuState,
                        0 // status description string index
                    };
                    co_await buffer.write(std::min(int(setup.wLength), int(sizeof(dfu::StatusReport))));

                    //if (dfuState == dfu::State::DFU_IDLE && downloader.enableJump) {
                    //    coco::system::jump(downloader.flashSections[appIndex].address);
                    //}
                }
                break;
            default:
                //debug::set(debug::MAGENTA);
                usb.stall();
            }
            break;
        case usb::RequestType::CLASS_INTERFACE_OUT:
            switch (setup.bRequest) {
            case dfu::Request::DFU_CLRSTATUS:
                //debug::out << "DFU_CLRSTATUS\n";
                usb.acknowledge();
                break;
            case dfu::Request::DFU_DETACH:
                usb.acknowledge();

                // jump to application after short delay to allow host to receive acknowledge
                co_await loop.sleep(10ms);
                //coco::system::jump(downloader.flashSections[appIndex].address);
                coco::system::reset(APP_RESET_INTENT);
                break;
            case dfu::Request::DFU_DNLOAD:
                // download firmware
                //debug::out << "DFU_DNLOAD wValue " << dec(setup.wValue) << " wLength " << dec(setup.wLength) << '\n';
                if (dfuState == dfu::State::DFU_IDLE || dfuState == dfu::State::DFU_DNLOAD_IDLE) {
                    int length = setup.wLength;
                    if (length > 0) {
                        // download from host
                        co_await buffer.read(length);

                        if (downloader.state == Downloader::State::FINISHED) {
                            // error: excess data
                            dfuState = dfu::State::DFU_ERROR;
                        } else {
                            dfuState = dfu::State::DFU_DNLOAD_SYNC;

                            // copy new data into global buffer
                            int toCopy = std::min(length, downloader.bufferSize - downloader.bufferPos);
                            std::copy(buffer.data(), buffer.data() + toCopy, downloader.buffer.u8 + downloader.bufferPos);
                            downloader.bufferPos += toCopy;

                            // notify coroutine (will consume most of the data in the buffer)
                            downloader.bufferBarrier.doFirst();

                            // copy tail
                            if (downloader.bufferPos + (length - toCopy) <= downloader.bufferSize) {
                                std::copy(buffer.data() + toCopy, buffer.data() + length, downloader.buffer.u8 + downloader.bufferPos);
                                downloader.bufferPos += length - toCopy;

                                // notify coroutine again
                                downloader.bufferBarrier.doFirst();
                            } else {
                                // error: buffer overflow (does not happen when host waits while we are in DFU_DNLOAD_SYNC state)
                                dfuState = dfu::State::DFU_ERROR;
                            }
                        }
                    } else {
                        // download finished
                        usb.acknowledge();
                        dfuState = (downloader.totalSize == downloader.bufferPos) ? dfu::State::DFU_MANIFEST_SYNC
                            : dfu::State::DFU_ERROR;
                    }
                } else {
                    // error: not expecting download
                    usb.stall();
                }
                break;
            case dfu::Request::DFU_ABORT:
                //debug::out << "DFU_ABORT\n";
                usb.acknowledge();
                switch (dfuState) {
                case dfu::State::DFU_DNLOAD_IDLE:
                    //! todo: abort download coroutine
                    dfuState = dfu::State::DFU_IDLE;
                    break;
                default:
                    ;
                }
                break;
            default:
                usb.stall();
            }
            break;
        case usb::RequestType::VENDOR_DEVICE_IN:
            switch (VendorRequest(setup.bRequest)) {
            case VendorRequest::WINUSB:
                // send WinUSB descriptor to host
                if (setup.wIndex == 0x07) {
                    //debug::out << "WinUSB\n";
                    co_await UsbDevice::controlIn(buffer, setup, winUsbDescriptor);
                } else {
                    usb.stall();
                }
                break;
            default:
                usb.stall();
            }
            break;
        default:
            usb.stall();
        }
    }
}

} // namespace coco
