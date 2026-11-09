/*
 * Firebat U6C: Quiet Fan / Office Mode for macOS
 *
 * Sets EC Fan Control Mode (FCMI) to Quiet Mode (0x82) on ACPI boot.
 * Under Windows, OEM software switches between 0x81 (Performance), 0x80 (Standard), 0x82 (Quiet).
 * On macOS, without OEM software, the EC defaults to 0x81 (Performance), keeping fans at high RPM.
 */
DefinitionBlock ("", "SSDT", 2, "DRIP", "FANQUIET", 0x00000000)
{
    External (_SB_.PCI0.SBRG.EC0_, DeviceObj)
    External (_SB_.PCI0.SBRG.EC0_.FCMI, FieldUnitObj)

    Scope (\_SB.PCI0.SBRG.EC0)
    {
        Method (_INI, 0, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                // 0x82 = Quiet / Silent fan profile in Firebat U6C EC
                FCMI = 0x82
            }
        }
    }
}
