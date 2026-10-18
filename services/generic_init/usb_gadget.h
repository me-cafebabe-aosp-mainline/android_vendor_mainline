/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

// A USB gadget made in first stage, for the system to run from storage on the USB host (tethered
// boot). Nothing else touches the gadget configuration after it: second stage init ignores the
// USB controller, so no USB HAL or USB init actions follow it.
namespace UsbGadget {

// Reads androidboot.usb_gadget=<function>, in first stage only. Unset, or with a function this
// does not know: off.
void Init();

// A UDC (USB device controller) appeared: configures the gadget if it is not yet, and binds it
void OnUdcAdd(const std::string& udc);

}  // namespace UsbGadget
