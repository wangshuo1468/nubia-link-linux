#pragma once

// Original SDK desktop roles. Type 10 selects RMCast in this ROM's PC list;
// type 7 selects ScreenCastSource and is also drawn as a PC by DeviceAdapter.
// Names, vendor and account remain the real Linux identity in both modes.
enum class DesktopRole { Collaboration = 7, SmartCast = 10 };
