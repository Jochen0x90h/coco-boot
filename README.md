# Coco Boot

Bootloader helper functions

## Import
Add coco-boot/\<version> to your conanfile where version corresponds to a git tag.

## Features
* USB DFU firmware update
* Write internal flash
* Update the bootloader itself (optional feature)

## Supported Platforms
* STM32 (needs CCMRAM on STM32G4 to update bootloader itself)
