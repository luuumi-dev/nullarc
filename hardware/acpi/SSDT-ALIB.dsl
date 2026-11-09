/*
 * Firebat U6C: AMD ALIB Power Event Fix for macOS (B-01).
 *
 * When a USB-C device is connected, the EC sends a power delivery event.
 * The firmware calls \_SB.ALIB(0x0C, Package) to adjust APU power limits.
 * Under macOS (AppleACPIPlatform / ACPICA), function 0x0C fails to return a value,
 * causing 5 consecutive AE_AML_NO_RETURN_VALUE errors and throttling the system.
 *
 * OpenCore patch:
 *   Comment: Rename ALIB to XLIB
 *   Find:    41 4C 49 42 (ALIB)
 *   Replace: 58 4C 49 42 (XLIB)
 */
DefinitionBlock ("", "SSDT", 2, "DRIP", "ALIBFIX", 0x00000000)
{
    External (_SB_.XLIB, MethodObj)

    Scope (_SB)
    {
        Method (ALIB, 2, Serialized)
        {
            If (_OSI ("Darwin"))
            {
                // Function 0x0C: APU power limit packet
                If (Arg0 == 0x0C)
                {
                    Return (Zero)
                }
            }

            Return (XLIB (Arg0, Arg1))
        }
    }
}
