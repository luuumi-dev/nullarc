// usbreenum: re-enumerates the USB device whose product name matches, as if it had been unplugged and plugged in again,
// so what follows a plug-in (B-01) can be measured without hands at the port.
// Usage (root): usbreenum <USB Product Name>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: usbreenum <USB Product Name>\n"); return 2; }
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &it)) return 1;
    int done = 0;
    for (io_service_t s; !done && (s = IOIteratorNext(it)); IOObjectRelease(s)) {
        char name[128] = "";
        CFStringRef n = IORegistryEntryCreateCFProperty(s, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
        if (n) { CFStringGetCString(n, name, sizeof name, kCFStringEncodingUTF8); CFRelease(n); }
        if (strcmp(name, argv[1])) continue;
        IOCFPlugInInterface **plug = NULL; IOUSBDeviceInterface **dev = NULL; SInt32 score;
        if (IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) || !plug) continue;
        (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
        (*plug)->Release(plug);
        if (!dev) continue;
        IOReturn r = (*dev)->USBDeviceOpen(dev);
        IOReturn e = (*dev)->USBDeviceReEnumerate(dev, 0);
        printf("%s: open 0x%x, re-enumerate 0x%x\n", name, r, e);
        if (r == kIOReturnSuccess) (*dev)->USBDeviceClose(dev);
        (*dev)->Release(dev);
        done = 1;
    }
    IOObjectRelease(it);
    return done ? 0 : 1;
}
