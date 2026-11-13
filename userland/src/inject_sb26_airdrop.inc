/*
 * inject_sb26_airdrop.inc: AirDrop mutex hang elimination, Screen Mirroring
 * asynchronous route discovery, and Calendar dynamic icon fallback.
 * Pure C99 implementation using ObjC runtime primitives.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <net/if.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>

/* --- AirDrop Hook Implementation --- */

static id (*orig_airdrop_init)(id self, SEL cmd);

static id
sb26_airdrop_init(id self, SEL cmd)
{
    /*
     * If the Apple Wireless Direct Link (awdl0) network interface is missing,
     * sharingd enters a deadlock/spin loop waiting for awdl0, causing
     * SFOperationCreate to block the SpringBoard main runloop on mutex [x19, #0x40].
     * Intercept -init to return an offline singleton without invoking SFOperationCreate.
     */
    if (if_nametoindex("awdl0") == 0) {
        static int logged_offline = 0;
        if (!logged_offline) {
            fprintf(stderr, "sb26: awdl0 absent, returning offline SFAirDropDiscoveryController\n");
            logged_offline = 1;
        }
        Class cls = object_getClass(self);
        Class super_cls = cls ? class_getSuperclass(cls) : NULL;
        if (super_cls) {
            struct objc_super sup;
            sup.receiver = self;
            sup.super_class = super_cls;
            id ret = ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, cmd);
            return ret ? ret : self;
        }
        return self;
    }
    if (orig_airdrop_init) {
        return orig_airdrop_init(self, cmd);
    }
    return self;
}

static BOOL
sb26_airdrop_is_available(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return NO;
}

static void
sb26_airdrop_set_discoverable_mode(id self, SEL cmd, long long mode)
{
    (void)self;
    (void)cmd;
    (void)mode;
}

static long long
sb26_airdrop_discoverable_mode(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return 0;
}

static BOOL
sb26_airdrop_is_action_disabled(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return YES;
}

static void
install_airdrop_hooks(Class cls)
{
    static int airdrop_done;
    if (airdrop_done || !cls) return;

    Method m_init = class_getInstanceMethod(cls, sel_registerName("init"));
    if (m_init) {
        orig_airdrop_init = (id (*)(id, SEL))method_getImplementation(m_init);
        method_setImplementation(m_init, (IMP)sb26_airdrop_init);
    }
    Method m_avail = class_getInstanceMethod(cls, sel_registerName("isAirDropAvailable"));
    if (m_avail) {
        method_setImplementation(m_avail, (IMP)sb26_airdrop_is_available);
    }
    Method m_set_mode = class_getInstanceMethod(cls, sel_registerName("setDiscoverableMode:"));
    if (m_set_mode) {
        method_setImplementation(m_set_mode, (IMP)sb26_airdrop_set_discoverable_mode);
    }
    Method m_get_mode = class_getInstanceMethod(cls, sel_registerName("discoverableMode"));
    if (m_get_mode) {
        method_setImplementation(m_get_mode, (IMP)sb26_airdrop_discoverable_mode);
    }
    Method m_disabled = class_getInstanceMethod(cls, sel_registerName("isActionDisabled"));
    if (m_disabled) {
        method_setImplementation(m_disabled, (IMP)sb26_airdrop_is_action_disabled);
    }

    airdrop_done = 1;
    fprintf(stderr, "sb26: SFAirDropDiscoveryController offline interception installed\n");
}

/* --- Screen Mirroring Asynchronous Discovery Implementation --- */

static id cached_output_devices = NULL;
static dispatch_once_t cached_dev_once;
static id (*orig_mru_available_devices)(id self, SEL cmd);
static id (*orig_av_available_devices)(id self, SEL cmd);
static void (*orig_mru_start_discovery)(id self, SEL cmd);

static void
sb26_update_cached_devices(void)
{
    dispatch_once(&cached_dev_once, ^{
        Class arr_cls = objc_getClass("NSArray");
        if (arr_cls) {
            id empty = ((id (*)(Class, SEL))objc_msgSend)(arr_cls, sel_registerName("array"));
            if (empty) {
                cached_output_devices = ((id (*)(id, SEL))objc_msgSend)(empty, sel_registerName("retain"));
            }
        }
    });
}

static id
sb26_output_available_devices(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    sb26_update_cached_devices();
    return cached_output_devices;
}

static void
sb26_mru_start_detailed_discovery(id self, SEL cmd)
{
    /*
     * MediaRemoteUI / MRUMirroringController must execute on the main thread
     * so that its delegate callbacks (-[MRUMirroringViewController mirroringController:...])
     * remain on the main thread, avoiding UIKit multithreading assertions.
     */
    Class thread_cls = objc_getClass("NSThread");
    BOOL on_main = thread_cls ? ((BOOL (*)(Class, SEL))objc_msgSend)(thread_cls, sel_registerName("isMainThread")) : YES;

    if (!on_main) {
        if (self) {
            ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("retain"));
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (orig_mru_start_discovery) {
                orig_mru_start_discovery(self, cmd);
            }
            sb26_update_cached_devices();
            if (self) {
                ((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("release"));
            }
        });
        return;
    }

    if (orig_mru_start_discovery) {
        orig_mru_start_discovery(self, cmd);
    }
    sb26_update_cached_devices();
}

static void
sb26_output_set_discovery_mode(id self, SEL cmd, long long mode)
{
    (void)self;
    (void)cmd;
    (void)mode;
}

static long long
sb26_output_discovery_mode(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return 0;
}

static BOOL
sb26_output_device_presence_detected(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return NO;
}

static void
sb26_routes_changed_cf_callback(CFNotificationCenterRef center, void *observer,
                                CFStringRef name, const void *object,
                                CFDictionaryRef userInfo)
{
    (void)center;
    (void)observer;
    (void)name;
    (void)object;
    (void)userInfo;
    sb26_update_cached_devices();
}

static void
install_screen_mirroring_hooks(Class cls)
{
    static int mirroring_done;
    if (mirroring_done || !cls) return;

    Method m_devs = class_getInstanceMethod(cls, sel_registerName("availableOutputDevices"));
    if (m_devs) {
        orig_av_available_devices = (id (*)(id, SEL))method_getImplementation(m_devs);
        method_setImplementation(m_devs, (IMP)sb26_output_available_devices);
    }
    Method m_set_mode = class_getInstanceMethod(cls, sel_registerName("setDiscoveryMode:"));
    if (m_set_mode) {
        method_setImplementation(m_set_mode, (IMP)sb26_output_set_discovery_mode);
    }
    Method m_get_mode = class_getInstanceMethod(cls, sel_registerName("discoveryMode"));
    if (m_get_mode) {
        method_setImplementation(m_get_mode, (IMP)sb26_output_discovery_mode);
    }
    Method m_presence = class_getInstanceMethod(cls, sel_registerName("devicePresenceDetected"));
    if (m_presence) {
        method_setImplementation(m_presence, (IMP)sb26_output_device_presence_detected);
    }

    /* Register notifications to drive route updates via CFNotificationCenter */
    CFNotificationCenterRef local = CFNotificationCenterGetLocalCenter();
    if (local) {
        CFNotificationCenterAddObserver(local, NULL, sb26_routes_changed_cf_callback,
                                        CFSTR("AVOutputDeviceDiscoverySessionAvailableOutputDevicesDidChangeNotification"),
                                        NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
        CFNotificationCenterAddObserver(local, NULL, sb26_routes_changed_cf_callback,
                                        CFSTR("AvailableRoutesChangedNotification"),
                                        NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    }

    mirroring_done = 1;
    fprintf(stderr, "sb26: AVOutputDeviceDiscoverySession async interception installed\n");
}

static void
install_mru_mirroring_hooks(Class cls)
{
    static int mru_done;
    if (mru_done || !cls) return;

    Method m_devs = class_getInstanceMethod(cls, sel_registerName("availableOutputDevices"));
    if (m_devs) {
        orig_mru_available_devices = (id (*)(id, SEL))method_getImplementation(m_devs);
        method_setImplementation(m_devs, (IMP)sb26_output_available_devices);
    }
    Method m_start = class_getInstanceMethod(cls, sel_registerName("startDetailedDiscovery"));
    if (m_start) {
        orig_mru_start_discovery = (void (*)(id, SEL))method_getImplementation(m_start);
        method_setImplementation(m_start, (IMP)sb26_mru_start_detailed_discovery);
    }

    mru_done = 1;
    fprintf(stderr, "sb26: MRUMirroringController async interception installed\n");
}

/* --- Calendar Dynamic Icon Fallback Implementation --- */

/*
 * SBIconImageInfo is an ARM64 Homogeneous Floating-point Aggregate (HFA) consisting
 * of 4 doubles: CGSize size (width, height), double scale, double continuousCornerRadius.
 * AAPCS64 passes it in d0..d3 floating-point registers. traitCollection is in x2 and options is in x3.
 */
struct SBIconImageInfo {
    CGSize size;
    double scale;
    double continuousCornerRadius;
};
typedef struct SBIconImageInfo SBIconImageInfo;

static id (*orig_calendar_layer)(id self, SEL cmd, SBIconImageInfo info, id tc, unsigned long opts);

/* Every icon provider in 23G90 answers -iconLayerWithInfo:traitCollection:options: with a
 * layer built by ICRIconLayer. That path comes back nil on this build (the icon services agent
 * still renders the artwork, but SpringBoard never gets a layer for it), which leaves every
 * SBIconImageView with contents=none: the home screen shows labels and no icons. v45 (no Metal
 * device, shouldForceBitmapImages forced YES) and v55 (Metal device, linked functions working)
 * both look the same, so this is not a Metal or a bitmap-selection problem.
 *
 * A provider that cannot make a layer can still make an image: -iconImageWithInfo:... answers an
 * ISImage with a CGImage. Wrap that CGImage in a plain CALayer, which is exactly what the
 * Calendar-only fallback below used to do for SBCalendarIconImageProvider. Here the same
 * fallback is installed on every class that implements the selector with Calendar's signature.
 * The original IMP is kept per class so subclasses that inherit the method still work.
 */
struct sb26_icon_orig {
    Class cls;
    IMP imp;
};
#define SB26_ICON_MAX 96
static struct sb26_icon_orig sb26_icon_origs[SB26_ICON_MAX];
static unsigned sb26_icon_orig_count;
static unsigned sb26_icon_hooked;
static unsigned sb26_icon_fallback_calls;
static unsigned sb26_icon_fallback_wrapped;
static unsigned sb26_icon_fallback_no_image;

static id
sb26_wrap_icon_image(id self, SBIconImageInfo info, id tc, unsigned long opts)
{
    SEL img_sel = sel_registerName("iconImageWithInfo:traitCollection:options:");
    Method m_img = class_getInstanceMethod(object_getClass(self), img_sel);
    if (!m_img) return nil;
    id (*img_fn)(id, SEL, SBIconImageInfo, id, unsigned long) =
        (id (*)(id, SEL, SBIconImageInfo, id, unsigned long))method_getImplementation(m_img);
    id image = img_fn(self, img_sel, info, tc, opts);
    if (!image) return nil;
    SEL cg_sel = sel_registerName("CGImage");
    if (!class_getInstanceMethod(object_getClass(image), cg_sel)) return nil;
    id cg_img = ((id (*)(id, SEL))objc_msgSend)(image, cg_sel);
    if (!cg_img) return nil;

    Class layer_cls = objc_getClass("CALayer");
    if (!layer_cls) return nil;
    /* An implicit CATransaction off the main thread asserts in QuartzCore. */
    Class tx_cls = objc_getClass("CATransaction");
    if (tx_cls) {
        ((void (*)(Class, SEL))objc_msgSend)(tx_cls, sel_registerName("begin"));
        ((void (*)(Class, SEL, BOOL))objc_msgSend)(tx_cls, sel_registerName("setDisableActions:"), YES);
    }
    id new_layer = ((id (*)(Class, SEL))objc_msgSend)(layer_cls, sel_registerName("layer"));
    if (new_layer) {
        ((void (*)(id, SEL, id))objc_msgSend)(new_layer, sel_registerName("setContents:"), cg_img);
        ((void (*)(id, SEL, id))objc_msgSend)(new_layer, sel_registerName("setContentsGravity:"),
            (id)CFSTR("resizeAspect"));
    }
    if (tx_cls) ((void (*)(Class, SEL))objc_msgSend)(tx_cls, sel_registerName("commit"));
    return new_layer;
}

static id
sb26_calendar_icon_layer_fallback(id self, SEL _cmd, SBIconImageInfo info, id tc, unsigned long opts)
{
    id layer = orig_calendar_layer ? orig_calendar_layer(self, _cmd, info, tc, opts) : nil;
    if (layer) return layer;
    sb26_icon_fallback_calls++;
    id wrapped = sb26_wrap_icon_image(self, info, tc, opts);
    if (wrapped) {
        sb26_icon_fallback_wrapped++;
        return wrapped;
    }
    sb26_icon_fallback_no_image++;
    return nil;
}

static IMP
sb26_icon_orig_for(id self)
{
    for (Class c = object_getClass(self); c; c = class_getSuperclass(c))
        for (unsigned i = 0; i < sb26_icon_orig_count; i++)
            if (sb26_icon_origs[i].cls == c) return sb26_icon_origs[i].imp;
    return NULL;
}

static id
sb26_generic_icon_layer(id self, SEL _cmd, SBIconImageInfo info, id tc, unsigned long opts)
{
    IMP orig = sb26_icon_orig_for(self);
    id layer = orig
        ? ((id (*)(id, SEL, SBIconImageInfo, id, unsigned long))orig)(self, _cmd, info, tc, opts)
        : nil;
    if (layer) {
        SEL contents_sel = sel_registerName("contents");
        if (!class_respondsToSelector(object_getClass(layer), contents_sel)) return layer;
        if (((id (*)(id, SEL))objc_msgSend)(layer, contents_sel)) return layer;
    }

    sb26_icon_fallback_calls++;
    id wrapped = sb26_wrap_icon_image(self, info, tc, opts);
    if (wrapped) {
        sb26_icon_fallback_wrapped++;
        return wrapped;
    }
    sb26_icon_fallback_no_image++;
    return layer;   /* keep whatever the original answered rather than making it worse */
}

/* Classes arrive lazily, so this is called once at constructor time and again a few times
 * during the first minute; already-hooked classes are left alone. */
void
sb26_install_all_icon_layer_fallbacks(void)
{
    SEL layer_sel = sel_registerName("iconLayerWithInfo:traitCollection:options:");
    Class calendar = objc_getClass("SBCalendarIconImageProvider");
    Method cm = calendar ? class_getInstanceMethod(calendar, layer_sel) : NULL;
    const char *want = cm ? method_getTypeEncoding(cm) : NULL;

    int n = objc_getClassList(NULL, 0);
    if (n <= 0) return;
    Class *classes = (Class *)calloc((size_t)n, sizeof(Class));
    if (!classes) return;
    n = objc_getClassList(classes, n);

    unsigned added = 0;
    for (int i = 0; i < n && sb26_icon_orig_count < SB26_ICON_MAX; i++) {
        Class cls = classes[i];
        if (!cls || cls == calendar) continue;   /* Calendar has its own hook */
        unsigned mcount = 0;
        Method *methods = class_copyMethodList(cls, &mcount);
        Method own = NULL;
        for (unsigned j = 0; j < mcount; j++)
            if (sel_isEqual(method_getName(methods[j]), layer_sel)) { own = methods[j]; break; }
        const char *enc = own ? method_getTypeEncoding(own) : NULL;
        free(methods);
        if (!own) continue;                       /* inherited: hooking the base is enough */
        if (want && (!enc || strcmp(enc, want) != 0)) continue;
        unsigned k;
        for (k = 0; k < sb26_icon_orig_count; k++)
            if (sb26_icon_origs[k].cls == cls) break;
        if (k < sb26_icon_orig_count) continue;
        IMP orig = method_getImplementation(own);
        if (orig == (IMP)sb26_generic_icon_layer || orig == (IMP)sb26_calendar_icon_layer_fallback)
            continue;
        sb26_icon_origs[sb26_icon_orig_count].cls = cls;
        sb26_icon_origs[sb26_icon_orig_count].imp = orig;
        sb26_icon_orig_count++;
        method_setImplementation(own, (IMP)sb26_generic_icon_layer);
        added++;
    }
    free(classes);
    sb26_icon_hooked = sb26_icon_orig_count;
    fprintf(stderr, "sb26: icon layer fallbacks: +%u hooked, %u total, enc=%s\n",
        added, sb26_icon_orig_count, want ? want : "unknown");
}

static void
install_calendar_icon_hooks(Class cls)
{
    static int cal_done;
    if (cal_done || !cls) return;

    Method m_layer = class_getInstanceMethod(cls, sel_registerName("iconLayerWithInfo:traitCollection:options:"));
    if (m_layer) {
        orig_calendar_layer = (id (*)(id, SEL, SBIconImageInfo, id, unsigned long))method_getImplementation(m_layer);
        method_setImplementation(m_layer, (IMP)sb26_calendar_icon_layer_fallback);
        cal_done = 1;
        fprintf(stderr, "sb26: SBCalendarIconImageProvider fallback hook installed\n");
    }
}
