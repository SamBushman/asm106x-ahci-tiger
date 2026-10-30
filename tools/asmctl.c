/* asmctl ro|rw : switch the loaded ASM106xAHCI driver between write-protected and writable (run as root) */
#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "ro") && strcmp(argv[1], "rw"))) { fprintf(stderr, "usage: asmctl ro|rw\n"); return 2; }
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ASM106xAHCI"));
    if (!s) { fprintf(stderr, "ASM106xAHCI not loaded\n"); return 1; }
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, CFSTR("ReadOnly"), strcmp(argv[1], "ro") == 0 ? kCFBooleanTrue : kCFBooleanFalse);
    kern_return_t kr = IORegistryEntrySetCFProperties(s, d);
    printf("%s -> 0x%x\n", argv[1], kr);
    return kr != KERN_SUCCESS;
}
