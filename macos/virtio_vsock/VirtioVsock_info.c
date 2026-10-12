/*
 * The kmod's identity, as VirtioNet's: the name and version kextload checks against the
 * Info.plist, and the start and stop routines of libkmodc++, which run the C++ constructors and
 * register the IOKit classes. No routine of the driver's own.
 */

#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(io.virtio.driver.VirtioVsock, VIRTIOVSOCK_VERSION, _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
