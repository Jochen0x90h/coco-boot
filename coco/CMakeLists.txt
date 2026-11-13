set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(${PROJECT_NAME})
target_sources(${PROJECT_NAME}
    PRIVATE
        Dummy.cpp
)

if(${PLATFORM} MATCHES "^stm32")
    target_sources(${PROJECT_NAME}
        PUBLIC FILE_SET headers TYPE HEADERS FILES
            Downloader.hpp
            DeviceInfo.hpp
            DfuBoot.hpp
        PRIVATE
            DfuBoot.cpp
    )

    target_link_libraries(${PROJECT_NAME}
        coco::coco
        coco-loop::coco-loop
        coco-device::coco-device
        coco-usb::coco-usb
    )

    target_include_directories(${PROJECT_NAME}
        PUBLIC
            ..
    )
endif()

# install the library
install(TARGETS ${PROJECT_NAME}
    FILE_SET headers DESTINATION include/coco
    FILE_SET platform_headers DESTINATION include
)
