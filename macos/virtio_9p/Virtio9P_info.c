/*
 * The kmod's identity, and its start and stop routines.
 *
 * Not libkmodc++'s _start and _stop as VirtioConsole has them: those run the C++ runtime's
 * setup and teardown and then _realmain/_antimain, so a stop routine that has to refuse --
 * while a 9p filesystem is mounted -- would refuse after the C++ classes were already gone
 * (libkern/kmod/cplus_stop.c). These do the same calls in the order this kext needs: start
 * sets up C++ (the IOKit driver's class) and then adds the filesystem; stop takes the
 * filesystem away first, and only then C++, and puts the filesystem back if C++ will not go
 * (a driver instance is still attached).
 */

#include <mach/mach_types.h>

extern int p9fs_register(void);
extern int p9fs_unregister(void);

#if defined(__x86_64__)
/*
 * The x86_64 kernels (10.6 on) set a kext's C++ up before its start routine and tear it down
 * after its stop routine has succeeded, and export neither call to kexts ("_OSRuntimeInitializeCPP
 * ... unresolved", live, 10.6.8 x86_64, 2026-10-10). So the order above holds by itself, and the
 * routines only add and take away the filesystem.
 */
static kern_return_t
virtio9p_start(kmod_info_t *ki, void *data)
{
    return p9fs_register() == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

static kern_return_t
virtio9p_stop(kmod_info_t *ki, void *data)
{
    return p9fs_unregister() == 0 ? KERN_SUCCESS : KERN_FAILURE;
}
#else
extern kern_return_t OSRuntimeInitializeCPP(kmod_info_t *ki, void *data);
extern kern_return_t OSRuntimeFinalizeCPP(kmod_info_t *ki, void *data);

static kern_return_t
virtio9p_start(kmod_info_t *ki, void *data)
{
    kern_return_t res = OSRuntimeInitializeCPP(ki, data);

    if (res != KERN_SUCCESS)
        return res;
    if (p9fs_register() != 0) {
        (void) OSRuntimeFinalizeCPP(ki, data);
        return KERN_FAILURE;
    }
    return KERN_SUCCESS;
}

static kern_return_t
virtio9p_stop(kmod_info_t *ki, void *data)
{
    kern_return_t res;

    if (p9fs_unregister() != 0)
        return KERN_FAILURE;
    res = OSRuntimeFinalizeCPP(ki, data);
    if (res != KERN_SUCCESS)
        (void) p9fs_register();
    return res;
}
#endif

KMOD_EXPLICIT_DECL(io.virtio.driver.Virtio9P, VIRTIO9P_VERSION, virtio9p_start, virtio9p_stop)

/* What libkmodc++'s _start and _stop, linked in for the C++ objects' sake, refer to. */
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
