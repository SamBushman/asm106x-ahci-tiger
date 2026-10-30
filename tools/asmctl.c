/* asmctl ro|rw : switch the loaded ASM106xAHCI driver between write-protected and writable (run as root) */
#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
int main(int argc, char **argv)
{
    int pw = (argc == 2 && (!strcmp(argv[1], "sleep") || !strcmp(argv[1], "wake")));
    int inj = (argc == 3 && !strcmp(argv[1], "inject") && (!strcmp(argv[2], "idnf") || !strcmp(argv[2], "timeout")));
    if (!inj && !pw && (argc != 2 || (strcmp(argv[1], "ro") && strcmp(argv[1], "rw")))) { fprintf(stderr, "usage: asmctl ro|rw | sleep|wake | inject idnf|timeout\n"); return 2; }
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ASM106xAHCI"));
    if (!s) { fprintf(stderr, "ASM106xAHCI not loaded\n"); return 1; }
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (pw) CFDictionarySetValue(d, CFSTR("Power"), CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingASCII));
    else if (inj) CFDictionarySetValue(d, CFSTR("Inject"), CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingASCII));
    else CFDictionarySetValue(d, CFSTR("ReadOnly"), strcmp(argv[1], "ro") == 0 ? kCFBooleanTrue : kCFBooleanFalse);
    kern_return_t kr = IORegistryEntrySetCFProperties(s, d);
    printf("%s -> 0x%x\n", argv[1], kr);
    return kr != KERN_SUCCESS;
}
