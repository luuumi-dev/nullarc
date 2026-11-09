/*
 * Firebat U6C: Embedded Controller Quiet Mode on boot for macOS
 */
DefinitionBlock ("", "SSDT", 2, "DRIP", "FANQUIET", 0x20251212)
{
    External (_SB_, DeviceObj)
    External (_SB_.PCI0.SBRG.EC0_, DeviceObj)
    External (_SB_.PCI0.SBRG.EC0_.FCMI, FieldUnitObj)

    Scope (_SB)
    {
        Device (FANQ)
        {
            Name (_HID, "DRIP0001")
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin"))
                {
                    // Write Quiet Mode profile directly to EC register FCMI
                    \_SB.PCI0.SBRG.EC0.FCMI = 0x82
                    Return (0x0F)
                }
                Else
                {
                    Return (Zero)
                }
            }
        }
    }
}
