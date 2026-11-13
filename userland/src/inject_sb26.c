/*
 * SpringBoard startup state, software icon selection and unlocking for 23G90.
 * MobileActivation is not implemented; this sets SBLockdownManager's local
 * activation state so SpringBoard can finish startup. The implementation and
 * object layout are specific to the checked 23G90 shared cache.
 *
 * The lock screen is dismissed once after boot, as 16.5's inject_sb.c does, and
 * again whenever the Darwin notification com.redwood.unlock is posted
 * (rwtouch notify com.redwood.unlock). autounlock=0 among the boot arguments
 * keeps the lock screen until it is swiped away.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <ptrauth.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#include <xpc/xpc.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

extern const void *_dyld_get_shared_cache_range(size_t *);
extern id MTLCreateSystemDefaultDevice(void);
void sb26_install_layer_dump(void);   /* inject_sb26_layers.c */
void sb26_install_rate(uintptr_t slide);   /* inject_sb26_rate.c */

static atomic_uint activation_calls;

static uintptr_t
code_address(IMP implementation)
{
    return (uintptr_t)ptrauth_strip(implementation, ptrauth_key_function_pointer);
}

static int
method_encoding_matches(Method method, const char *expected)
{
    const char *encoding = method ? method_getTypeEncoding(method) : NULL;
    return encoding && !strcmp(encoding, expected);
}

static Method
checked_method(const char *class_name, const char *selector_name, const char *encoding,
    uintptr_t expected, const unsigned char bytes[16])
{
    Class cls = objc_getClass(class_name);
    Method method = cls ? class_getInstanceMethod(cls, sel_registerName(selector_name)) : NULL;
    IMP implementation = method ? method_getImplementation(method) : NULL;
    uintptr_t address = implementation ? code_address(implementation) : 0;
    int match = address == expected && method_encoding_matches(method, encoding);
    if (match) match = !memcmp((const void *)address, bytes, 16);
    fprintf(stderr, "sb26: validate %s address=%lx expected=%lx encoding=%s match=%d\n",
        selector_name, address, expected, method ? method_getTypeEncoding(method) : "missing", match);
    return match ? method : NULL;
}

static IMP original_bitmap_selection;
static dispatch_once_t metal_device_once;
static int has_metal_device;
static SEL appearance_selector, glass_selector;

static void
query_metal_device(void *context)
{
    (void)context;
    id device = MTLCreateSystemDefaultDevice();
    has_metal_device = device != nil;
    /* MTLCreateSystemDefaultDevice returns a retained object. */
    if (device) ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("release"));
    fprintf(stderr, "sb26: icon rendering Metal device=%d\n", has_metal_device);
}

static BOOL
software_bitmap_selection(id self, SEL selector)
{
	BOOL original = ((BOOL (*)(id, SEL))original_bitmap_selection)(self, selector);
	if (original) {
		return original;
	}

	/*
	 * If the current icon view carries a Calendar icon (e.g. SBHCalendarApplicationIcon),
	 * force bitmap rasterization. In iOS 26 with Liquid Glass enabled, SBCalendarIconImageProvider
	 * attempts to invoke [ISImage ICRIconLayer], which is unimplemented and returns nil,
	 * leaving CALayer.contents empty and making the icon transparent.
	 */
	id icon = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("icon"));
	if (icon != nil) {
		const char *icon_class = object_getClassName(icon);
		if (icon_class && strstr(icon_class, "Calendar") != NULL) {
			return YES;
		}
	}

	Method appearance_method = class_getInstanceMethod(object_getClass(self), appearance_selector);
	if (!method_encoding_matches(appearance_method, "@16@0:8")) {
		return original;
	}
	id appearance = ((id (*)(id, SEL))objc_msgSend)(self, appearance_selector);
	Method glass = appearance ? class_getInstanceMethod(object_getClass(appearance), glass_selector) : NULL;
	if (!method_encoding_matches(glass, "B16@0:8")) {
		return original;
	}
	if (((BOOL (*)(id, SEL))objc_msgSend)(appearance, glass_selector)) {
		return original;
	}

	dispatch_once_f(&metal_device_once, NULL, query_metal_device);
	return has_metal_device ? original : YES;
}

static void
install_bitmap_selection(uintptr_t slide)
{
    static const unsigned char bytes[16] = {
        0x7f,0x23,0x03,0xd5,0xf6,0x57,0xbd,0xa9,0xf4,0x4f,0x01,0xa9,0xfd,0x7b,0x02,0xa9
    };
    Method method = checked_method("SBIconImageView", "shouldForceBitmapImages", "B16@0:8",
        0x1c1a37ae0UL + slide, bytes);
    Method appearance = checked_method("SBIconImageView", "effectiveIconImageAppearance", "@16@0:8",
        0x1c17cc92cUL + slide, bytes);
    if (!method || !appearance) return;
    appearance_selector = sel_registerName("effectiveIconImageAppearance");
    glass_selector = sel_registerName("hasGlass");
    original_bitmap_selection = method_getImplementation(method);
    method_setImplementation(method, (IMP)software_bitmap_selection);
    int installed = code_address(method_getImplementation(method)) == code_address((IMP)software_bitmap_selection);
    fprintf(stderr, "sb26: build=23G90 software_icons_installed=%d\n", installed);
}

static void
setup_activation_state(id self, SEL selector)
{
    (void)selector;
    int previous;
    memcpy(&previous, (const char *)self + 8, sizeof(previous));
    int activated = 2;
    memcpy((char *)self + 8, &activated, sizeof(activated));
    unsigned call = atomic_fetch_add_explicit(&activation_calls, 1, memory_order_relaxed);
    if (call < 16)
        fprintf(stderr, "sb26: activation call=%u previous=%d applied=2\n", call + 1, previous);
}

/* What the cover sheet passes to -[SBLockScreenManager unlockUIFromSource:withOptions:] on 16.5. */
#define UNLOCK_SOURCE_COVER_SHEET 24
/*
 * The lock screen takes input about 21 s after SpringBoard connects to backboardd: on 2026-10-29
 * the boot overlay came down 18 s in and the backlight stayed off 3 s more, and two swipes sent
 * before that were dropped. isUILocked is not yet true at the first attempts either, so the boot
 * unlock is done only once the lock screen has been seen up and then gone.
 */
#define UNLOCK_FIRST_SECONDS 4
#define UNLOCK_EVERY_SECONDS 1
#define UNLOCK_ATTEMPTS 60

static id
lock_screen_manager(void)
{
    Class cls = objc_getClass("SBLockScreenManager");
    SEL shared = sel_registerName("sharedInstance");
    if (!cls || !class_getClassMethod(cls, shared)) return nil;
    return ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
}

/* 1: locked, an unlock was asked for; 0: not locked; -1: no lock screen manager to ask. */

static int
unlock_once(const char *why)
{
    SEL locked = sel_registerName("isUILocked");
    SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
    Class cls = objc_getClass("SBLockScreenManager");
    if (!cls || !class_getInstanceMethod(cls, locked) || !class_getInstanceMethod(cls, unlock_ui)) {
        fprintf(stderr, "sb26: unlock %s: SBLockScreenManager lacks isUILocked or unlockUIFromSource:withOptions:\n", why);
        return -1;
    }
    id manager = lock_screen_manager();
    if (!manager) return -1;
    if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
        notify_post("com.redwood.wallpaper.suspend");
        return 0;
    }
    BOOL answer = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, nil);
    int locked_after = ((BOOL (*)(id, SEL))objc_msgSend)(manager, locked);
    fprintf(stderr, "sb26: unlock %s: answer=%d locked_after=%d\n", why, answer, locked_after);
    if (!locked_after) {
        notify_post("com.redwood.wallpaper.suspend");
    }
    return 1;
}

static int boot_unlock_seen_locked;

/* v62: the stepped "updateImageAnimated:/setNeedsLayout" refresh chain is gone. It was
 * added for the v50 icon-black problem, which the app-bundle PNG fallback now solves, and
 * it re-laid out every visible window 16 times in the first 150 s -- exactly the period the
 * user reports as "cannot swipe at first, then later it scrolls fine". */

static void
boot_unlock(void *context)
{
    uintptr_t attempt = (uintptr_t)context + 1;
    int state = unlock_once("boot");
    if (state == 1) boot_unlock_seen_locked = 1;
    if (state == 0 && boot_unlock_seen_locked) {
        fprintf(stderr, "sb26: boot unlock done after %lu attempts\n", (unsigned long)attempt);
        notify_post("com.redwood.wallpaper.suspend");
        return;
    }
    if (attempt >= UNLOCK_ATTEMPTS) {
        fprintf(stderr, "sb26: boot unlock gave up after %lu attempts, lock screen seen=%d\n",
            (unsigned long)attempt, boot_unlock_seen_locked);
        return;
    }
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_EVERY_SECONDS * NSEC_PER_SEC),
        dispatch_get_main_queue(), (void *)attempt, boot_unlock);
}

static BOOL (*orig_unlock_ui)(id, SEL, long, id);
static BOOL
sb26_hook_unlock_ui(id self, SEL cmd, long source, id options)
{
    BOOL ans = orig_unlock_ui ? orig_unlock_ui(self, cmd, source, options) : NO;
    SEL locked = sel_registerName("isUILocked");
    if (!((BOOL (*)(id, SEL))objc_msgSend)(self, locked)) {
        notify_post("com.redwood.wallpaper.suspend");
    }
    return ans;
}

int
sb26_boot_arg_present(const char *wanted)
{
    char args[1024] = "";
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) return 0;
    size_t length = strlen(wanted);
    for (const char *at = strstr(args, wanted); at; at = strstr(at + 1, wanted))
        if ((at == args || at[-1] == ' ') && (at[length] == ' ' || at[length] == 0)) return 1;
    return 0;
}

static void
install_unlock(void)
{
    int token;
    notify_register_dispatch("com.redwood.unlock", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        if (unlock_once("notification") == 0) {
            fprintf(stderr, "sb26: unlock notification: not locked\n");
            notify_post("com.redwood.wallpaper.suspend");
        }
    });

    Class cls = objc_getClass("SBLockScreenManager");
    if (cls) {
        Method m = class_getInstanceMethod(cls, sel_registerName("unlockUIFromSource:withOptions:"));
        if (m) {
            orig_unlock_ui = (BOOL (*)(id, SEL, long, id))method_getImplementation(m);
            method_setImplementation(m, (IMP)sb26_hook_unlock_ui);
        }
    }

    if (sb26_boot_arg_present("autounlock=0")) {
        fprintf(stderr, "sb26: autounlock=0, the lock screen stays until it is swiped away\n");
        return;
    }
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_FIRST_SECONDS * NSEC_PER_SEC),
        dispatch_get_main_queue(), NULL, boot_unlock);
}

static BOOL
haptic_return_no(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return NO;
}

static BOOL
haptic_start_and_return_error(id self, SEL cmd, id *err)
{
    (void)self;
    (void)cmd;
    if (err) *err = nil;
    return YES;
}

static void
haptic_start_with_completion(id self, SEL cmd, void (^handler)(id))
{
    (void)self;
    (void)cmd;
    if (handler) handler(nil);
}

static BOOL
haptic_stop_and_return_error(id self, SEL cmd, id *err)
{
    (void)self;
    (void)cmd;
    if (err) *err = nil;
    return YES;
}

static void
haptic_stop_with_completion(id self, SEL cmd, void (^handler)(id))
{
    (void)self;
    (void)cmd;
    if (handler) handler(nil);
}

static id (*orig_haptic_caps)(id, SEL);
static id
haptic_capabilities_for_hardware(id self, SEL cmd)
{
    id caps = orig_haptic_caps ? orig_haptic_caps(self, cmd) : nil;
    if (caps) {
        Class caps_cls = object_getClass(caps);
        Method m_h = class_getInstanceMethod(caps_cls, sel_registerName("supportsHaptics"));
        if (m_h) method_setImplementation(m_h, (IMP)haptic_return_no);
        Method m_a = class_getInstanceMethod(caps_cls, sel_registerName("supportsAudio"));
        if (m_a) method_setImplementation(m_a, (IMP)haptic_return_no);
    }
    return caps;
}

static void
install_haptic_hooks(Class engine_cls)
{
    if (!engine_cls) return;

    Method m_caps = class_getClassMethod(engine_cls, sel_registerName("capabilitiesForHardware"));
    if (m_caps) {
        orig_haptic_caps = (id (*)(id, SEL))method_getImplementation(m_caps);
        method_setImplementation(m_caps, (IMP)haptic_capabilities_for_hardware);
    }

    Method m_start = class_getInstanceMethod(engine_cls, sel_registerName("startAndReturnError:"));
    if (m_start) method_setImplementation(m_start, (IMP)haptic_start_and_return_error);

    Method m_start_cb = class_getInstanceMethod(engine_cls, sel_registerName("startWithCompletionHandler:"));
    if (m_start_cb) method_setImplementation(m_start_cb, (IMP)haptic_start_with_completion);

    Method m_stop = class_getInstanceMethod(engine_cls, sel_registerName("stopAndReturnError:"));
    if (m_stop) method_setImplementation(m_stop, (IMP)haptic_stop_and_return_error);

    Method m_stop_cb = class_getInstanceMethod(engine_cls, sel_registerName("stopWithCompletionHandler:"));
    if (m_stop_cb) method_setImplementation(m_stop_cb, (IMP)haptic_stop_with_completion);

    fprintf(stderr, "sb26: CHHapticEngine non-blocking interception installed\n");
}

static void
haptic_image_callback(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    static int haptics_done;
    if (haptics_done) return;
    Class cls = objc_getClass("CHHapticEngine");
    if (cls) {
        haptics_done = 1;
        install_haptic_hooks(cls);
    }
}

static int
is_build_23g90(void)
{
    FILE *file = fopen("/System/Library/CoreServices/SystemVersion.plist", "rb");
    if (!file) return 0;
    UInt8 bytes[8192];
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    int valid = !ferror(file) && size && size < sizeof(bytes);
    fclose(file);
    if (!valid) return 0;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)size);
    if (!data) return 0;
    CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
        kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    valid = 0;
    if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        CFTypeRef build = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("ProductBuildVersion"));
        valid = build && CFGetTypeID(build) == CFStringGetTypeID() && CFEqual(build, CFSTR("23G90"));
    }
    if (plist) CFRelease(plist);
    return valid;
}

/*
 * Settings "About" capacity fix:
 * Preferences reads total capacity from MGCopyAnswer(CFSTR("DiskUsage")) or
 * MGCopyAnswer(CFSTR("TotalDiskCapacity")). Because HackiOS lacks Apple NVMe
 * controller driver, MobileGestalt returns 0 KB.
 * We interpose MGCopyAnswer to report a standard 256 GB capacity with
 * real free space read from statfs("/private/var").
 */
extern CFTypeRef MGCopyAnswer(CFStringRef property);

static void
sb26_query_capacities(int64_t *total_disk, int64_t *total_sys, int64_t *total_data, int64_t *avail_val)
{
    uint64_t sys_b = 0, data_b = 0, used_b = 0;
    size_t sz = sizeof(uint64_t);
    struct statfs st;

    (void)sysctlbyname("kern.redwood.darwin_bytes", &sys_b, &sz, NULL, 0);
    sz = sizeof(uint64_t);
    (void)sysctlbyname("kern.redwood.darwin_var_bytes", &data_b, &sz, NULL, 0);
    if (sys_b == 0) sys_b = 8ULL << 30;
    if (data_b == 0) data_b = 112ULL << 30;
    if (statfs("/private/var", &st) == 0 && st.f_blocks > st.f_bavail) {
        used_b = (uint64_t)(st.f_blocks - st.f_bavail) * (uint64_t)st.f_bsize;
    }
    *total_sys  = (int64_t)sys_b;
    *total_data = (int64_t)data_b;
    *total_disk = (int64_t)(sys_b + data_b);
    *avail_val  = (int64_t)(data_b > used_b ? data_b - used_b : data_b);
}

static CFTypeRef
sb26_hook_MGCopyAnswer(CFStringRef property)
{
    const char *prog = getprogname();
    if (property && prog && (strcmp(prog, "Preferences") == 0 || strcmp(prog, "SpringBoard") == 0)) {
        int64_t total_disk = 0, total_sys = 0, total_data = 0, avail_val = 0;
        if (CFEqual(property, CFSTR("DiskUsage"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);

            CFNumberRef num_total_disk = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_disk);
            CFNumberRef num_total_sys  = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_sys);
            CFNumberRef num_total_data = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_data);
            CFNumberRef num_avail      = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &avail_val);

            const void *keys[] = {
                CFSTR("TotalDiskCapacity"),
                CFSTR("TotalSystemCapacity"),
                CFSTR("TotalDataCapacity"),
                CFSTR("TotalDataAvailable"),
                CFSTR("AmountDataAvailable")
            };
            const void *values[] = {
                num_total_disk,
                num_total_sys,
                num_total_data,
                num_avail,
                num_avail
            };

            CFDictionaryRef dict = CFDictionaryCreate(
                kCFAllocatorDefault,
                keys,
                values,
                sizeof(keys) / sizeof(keys[0]),
                &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks
            );

            if (num_total_disk) CFRelease(num_total_disk);
            if (num_total_sys)  CFRelease(num_total_sys);
            if (num_total_data) CFRelease(num_total_data);
            if (num_avail)      CFRelease(num_avail);

            return dict;
        }

        if (CFEqual(property, CFSTR("TotalDiskCapacity"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);
            return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_disk);
        }

        if (CFEqual(property, CFSTR("TotalSystemCapacity"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);
            return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_sys);
        }

        if (CFEqual(property, CFSTR("TotalDataCapacity"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);
            return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_data);
        }

        if (CFEqual(property, CFSTR("TotalDataAvailable")) || CFEqual(property, CFSTR("AmountDataAvailable"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);
            return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &avail_val);
        }

        if (CFEqual(property, CFSTR("airdrop-p2p")) || CFEqual(property, CFSTR("airdrop"))) {
            return kCFBooleanFalse;
        }
    }

    return MGCopyAnswer(property);
}

/*
 * CoreFoundation bundle localization fix:
 * In pruned images (select-system.py), zh*.lproj directories are absent,
 * causing CFBundleCopyBundleLocalizations to assume the bundle only supports English.
 * System .loctable files retain Chinese translation entries.
 * We interpose CFBundleCopyBundleLocalizations to append Chinese language identifiers
 * whenever the bundle has localization resources (.loctable or strings).
 */
extern CFArrayRef CFBundleCopyBundleLocalizations(CFBundleRef bundle);

static int
dir_contains_localization_file(const char *dirpath)
{
    DIR *d = opendir(dirpath);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        size_t len = strlen(name);
        if ((len > 9 && !strcmp(name + len - 9, ".loctable")) ||
            (len > 8 && !strcmp(name + len - 8, ".strings")) ||
            (len > 12 && !strcmp(name + len - 12, ".stringsdict")) ||
            (len > 6 && !strcmp(name + len - 6, ".lproj"))) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int
bundle_has_localization_resources(CFBundleRef bundle)
{
    if (!bundle) return 0;
    CFURLRef url = CFBundleCopyBundleURL(bundle);
    if (!url) return 1;
    char path[PATH_MAX], sub[PATH_MAX];
    Boolean ok = CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof(path));
    CFRelease(url);
    if (!ok) return 1;
    if (dir_contains_localization_file(path)) return 1;
    if (snprintf(sub, sizeof(sub), "%s/Resources", path) > 0 && dir_contains_localization_file(sub)) return 1;
    if (snprintf(sub, sizeof(sub), "%s/Contents/Resources", path) > 0 && dir_contains_localization_file(sub)) return 1;
    return 0;
}

static CFArrayRef
sb26_hook_CFBundleCopyBundleLocalizations(CFBundleRef bundle)
{
    CFArrayRef orig = CFBundleCopyBundleLocalizations(bundle);
    if (!orig || CFGetTypeID(orig) != CFArrayGetTypeID()) return orig;
    CFIndex count = CFArrayGetCount(orig);
    if (count <= 0) return orig;

    for (CFIndex i = 0; i < count; i++) {
        CFTypeRef item = CFArrayGetValueAtIndex(orig, i);
        if (item && CFGetTypeID(item) == CFStringGetTypeID() && CFStringHasPrefix((CFStringRef)item, CFSTR("zh"))) {
            return orig;
        }
    }
    if (!bundle_has_localization_resources(bundle)) return orig;

    static const CFStringRef zh_codes[] = {
        CFSTR("zh-Hans"), CFSTR("zh_CN"), CFSTR("zh-Hant"), CFSTR("zh_TW"), CFSTR("zh_HK"),
    };
    const CFIndex num_zh = (CFIndex)(sizeof(zh_codes) / sizeof(zh_codes[0]));
    CFMutableArrayRef mut = CFArrayCreateMutableCopy(kCFAllocatorDefault, count + num_zh, orig);
    if (!mut) return orig;

    for (CFIndex i = 0; i < num_zh; i++) {
        CFStringRef code = zh_codes[i];
        int exists = 0;
        CFIndex cur = CFArrayGetCount(mut);
        for (CFIndex j = 0; j < cur; j++) {
            CFTypeRef v = CFArrayGetValueAtIndex(mut, j);
            if (v && CFGetTypeID(v) == CFStringGetTypeID() && CFEqual(v, code)) { exists = 1; break; }
        }
        if (!exists) CFArrayAppendValue(mut, code);
    }
    CFRelease(orig);
    return (CFArrayRef)mut;
}

/*
 * Ensure .GlobalPreferences.plist in /private/var/mobile/Library/Preferences/
 * has AppleLanguages with "zh-Hans-CN" prioritized.
 */
static void
sb26_ensure_chinese_preferences(void)
{
    static int preferences_ensured = 0;
    if (preferences_ensured) return;
    preferences_ensured = 1;

    const char *pref_dir = "/private/var/mobile/Library/Preferences";
    const char *pref_path = "/private/var/mobile/Library/Preferences/.GlobalPreferences.plist";

    struct stat st;
    if (stat(pref_dir, &st) != 0) {
        mkdir("/private/var/mobile", 0755);
        chown("/private/var/mobile", 501, 501);
        mkdir("/private/var/mobile/Library", 0755);
        chown("/private/var/mobile/Library", 501, 501);
        mkdir(pref_dir, 0755);
        chown(pref_dir, 501, 501);
    }

    CFMutableDictionaryRef dict = NULL;
    int fd = open(pref_path, O_RDONLY);
    if (fd >= 0) {
        if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 1024 * 1024) {
            void *buf = malloc((size_t)st.st_size);
            if (buf) {
                if (read(fd, buf, (size_t)st.st_size) == st.st_size) {
                    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buf, st.st_size, kCFAllocatorMalloc);
                    if (data) {
                        CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListMutableContainersAndLeaves, NULL, NULL);
                        CFRelease(data);
                        if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) dict = (CFMutableDictionaryRef)plist;
                        else if (plist) CFRelease(plist);
                    }
                } else {
                    free(buf);
                }
            }
        }
        close(fd);
    }
    if (!dict) dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!dict) return;

    CFArrayRef orig_langs = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("AppleLanguages"));
    int need_update = 0;
    if (!orig_langs || CFGetTypeID(orig_langs) != CFArrayGetTypeID() || CFArrayGetCount(orig_langs) == 0) {
        need_update = 1;
    } else {
        CFTypeRef first = CFArrayGetValueAtIndex(orig_langs, 0);
        if (!first || CFGetTypeID(first) != CFStringGetTypeID() ||
            (!CFEqual(first, CFSTR("zh-Hans-CN")) && !CFEqual(first, CFSTR("zh-Hans")))) {
            need_update = 1;
        }
    }

    if (need_update) {
        CFMutableArrayRef new_langs = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(new_langs, CFSTR("zh-Hans-CN"));
        CFArrayAppendValue(new_langs, CFSTR("zh-Hans"));
        if (orig_langs && CFGetTypeID(orig_langs) == CFArrayGetTypeID()) {
            CFIndex count = CFArrayGetCount(orig_langs);
            for (CFIndex i = 0; i < count; i++) {
                CFTypeRef item = CFArrayGetValueAtIndex(orig_langs, i);
                if (item && CFGetTypeID(item) == CFStringGetTypeID() &&
                    !CFEqual(item, CFSTR("zh-Hans-CN")) && !CFEqual(item, CFSTR("zh-Hans"))) {
                    CFArrayAppendValue(new_langs, item);
                }
            }
        } else {
            CFArrayAppendValue(new_langs, CFSTR("en-CN"));
            CFArrayAppendValue(new_langs, CFSTR("en"));
        }
        CFDictionarySetValue(dict, CFSTR("AppleLanguages"), new_langs);
        if (!CFDictionaryContainsKey(dict, CFSTR("AppleLocale"))) {
            CFDictionarySetValue(dict, CFSTR("AppleLocale"), CFSTR("zh_CN"));
        }

        CFDataRef xml_data = CFPropertyListCreateData(kCFAllocatorDefault, dict, kCFPropertyListXMLFormat_v1_0, 0, NULL);
        if (xml_data) {
            int wfd = open(pref_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (wfd >= 0) {
                ssize_t written = write(wfd, CFDataGetBytePtr(xml_data), (size_t)CFDataGetLength(xml_data));
                (void)written;
                close(wfd);
                chown(pref_path, 501, 501);
                chmod(pref_path, 0644);
            }
            CFRelease(xml_data);
        }
        CFPreferencesSetAppValue(CFSTR("AppleLanguages"), new_langs, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("AppleLocale"), CFSTR("zh_CN"), kCFPreferencesAnyApplication);
        CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
        CFRelease(new_langs);
        fprintf(stderr, "sb26: AppleLanguages ensured zh-Hans-CN prioritized (updated)\n");
    }
    CFRelease(dict);
}


typedef struct { double x, y; } sb26_pt;

/* ---------------------------------------------------------------------------------------
 * v65: iconservicesagent has died generation after generation (pid 60, 84, 86, 93, 99, 105,
 * and in v61/v62 alike) with NO abort message on stderr, always in the middle of our Metal
 * shim's work, and every death strands SpringBoard's sync XPC request forever: the tap-an-icon
 * freeze. Print a backtrace on fatal signals so the crash site finally shows up on serial.
 * --------------------------------------------------------------------------------------- */
static void
sb26_crash_logger(int number, siginfo_t *info, void *context)
{
    void *frames[32];
    char line[1100];
    int n = backtrace(frames, 32), k;

    (void)context;
    k = snprintf(line, sizeof(line), "sb26 CRASH: %s: SIGNAL %d at %p, the stack:",
        getprogname() ? getprogname() : "?", number, info ? info->si_addr : NULL);
    for (int i = 0; i < n && k < (int)sizeof(line) - 24; i++) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " %p", frames[i]);
    }
    line[k++] = '\n';
    (void)write(2, line, (size_t)k);
    signal(number, SIG_DFL);
    raise(number);
}

/* Installed in EVERY process the dylib reaches, before the SpringBoard-only early return. */
static void
sb26_install_crash_logger(void)
{
    static const int signals[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };
    struct sigaction action = { 0 };

    action.sa_sigaction = sb26_crash_logger;
    action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        sigaction(signals[i], &action, NULL);
}

/* ---------------------------------------------------------------------------------------
 * v64: SpringBoard's main thread freezes on an icon tap inside
 *   mach_msg2_trap -> dispatch_mach_send_with_result_and_wait_for_reply ->
 *   xpc_connection_send_message_with_reply_sync
 * and never returns (v62: the same stack from t=94 to t=1991).
 *
 * v63 tried to replace that call with an async reply + semaphore and crashed SpringBoard four
 * times with "-[NSXPCConnection _sendInvocation:...]: encoded data has been corrupted":
 * NSXPCConnection owns the reply encoding, so an interposer must NOT change the call's shape.
 * v64 therefore only logs, in exactly the style of sb26_hook_CFBundleCopyBundleLocalizations
 * above (calling the original symbol from the interposer does not recurse here).
 *
 * Every "->" line is printed BEFORE the call; the one with no matching "<-" is the service
 * SpringBoard is stuck on. A thread-local depth guard keeps a hypothetical recursion from
 * turning into a stack overflow.
 *
 * v64 on the phone: the never-answered requests are com.apple.iconservices — the
 * iconservicesagent process dies (EXC_CORPSE_NOTIFY, pid 60/84/86/93/99/105, no stderr
 * output, always mid-Metal-shim) with SpringBoard's sync request already on the wire,
 * which strands the waiting thread forever; enough strands take down the main thread.
 * v65 dumped every iconservices message (ISGenerationRequest, replysig
 * "v16@?0@\"ISGenerationResponse\"8") and installed a fatal-signal backtrace logger;
 * on the phone the logger stayed silent for the agent, so the agent dies through a
 * path a signal handler cannot see (SIGKILL or a direct mach exception).
 * v66 bounded the wait to 4 s but returned NULL on timeout, and SpringBoard died at
 * once: NSXPCConnection feeds the interposer's return value straight into its reply
 * decoder ("root object is not an XPC dictionary"). v67 caches real replies keyed by
 * the request's "root" blob and replays them on timeout; with a cold cache an empty
 * dictionary goes back instead. Nothing ever returns NULL.
 * --------------------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------------------
 * v66 verdict: the 4 s bound works, but a NULL reply kills SpringBoard immediately:
 *   NSInvalidArgumentException: "... encoded data has been corrupted, root object is
 *   not an XPC dictionary"  — NSXPCConnection hands the interposer's return value
 *   straight to the reply decoder, which requires a dictionary.
 * v67 therefore caches every real iconservices reply keyed by the request's "root"
 * blob; on timeout the cached reply is replayed, and if the cache is cold an empty
 * dictionary is returned. Nothing ever returns NULL.
 * v68 keeps the reply the async API itself hands back for a dead peer — an
 * XPC_TYPE_ERROR object — instead of filtering it down to NULL (v67 died on that).
 * --------------------------------------------------------------------------------------- */
#include "inject_sb26_xpc_compat.inc"

/* Key: length + first 64 bytes of the request's "root" data — icon requests for the
 * same icon repeat byte-for-byte (the "sequence" number lives outside the blob). */
static void
sb26_xpc_cache_key(xpc_object_t message, char *key, size_t keysz)
{
    key[0] = '\0';
    xpc_object_t root = message ? xpc_dictionary_get_value(message, "root") : NULL;
    if (!root || xpc_get_type(root) != XPC_TYPE_DATA) {
        snprintf(key, keysz, "no-root");
        return;
    }
    size_t len = xpc_data_get_length(root);
    const void *bytes = xpc_data_get_bytes_ptr(root);
    unsigned n = len < 64 ? (unsigned)len : 64;
    int k = snprintf(key, keysz, "%zu:", len);
    for (unsigned i = 0; i < n && k < (int)keysz - 2; i++)
        k += snprintf(key + k, keysz - (size_t)k, "%02x",
            ((const unsigned char *)bytes)[i]);
}

/* v69: a decodable reply, any reply. When the agent dies with a request in flight the async
 * API can hand back NULL (v68: two of those still reached NSXPCConnection and aborted
 * SpringBoard). A cached ISGenerationResponse for ANOTHER icon decodes fine and shows that
 * icon; a crash shows nothing. Last resort only, and only ever a real captured reply. */
/* ---------------------------------------------------------------------------------------
 * v84: the trimmed system has Mach services with no launchd job behind them. Every
 * synchronous request to one of them comes back with the SAME global XPC_TYPE_ERROR
 * object (v82: 0x20e3f2460 for commcenter.xpc x255, appprotectiond.read x204 and
 * splashboard.launchimage x2; v83 added coremedia.routediscoverer.xpc and
 * audio.AudioSession). SpringBoard makes a fresh bootstrap lookup for each of them,
 * and a single icon tap issued 32 appprotectiond.read calls in v82.
 *
 * After a service has answered with the identical error object three times in a
 * row it is permanently dead: keep that object and hand it straight back. The
 * object is a global XPC error constant, so the answer is exactly what the real
 * call would have produced, minus the launchd lookup. Only XPC_TYPE_ERROR replies
 * are ever cached; any other reply clears the streak, so a live service can never
 * be short-circuited. iconservices is excluded -- its reply replay/cache logic
 * above must keep seeing every request.
 * --------------------------------------------------------------------------------------- */
#define SB26_DEAD_MAX       24
#define SB26_DEAD_STREAK    3
static struct {
    char name[96];
    xpc_object_t err;               /* cached error, retained */
    xpc_object_t last;              /* last error pointer seen, not retained */
    unsigned streak;
    unsigned long hits;
} sb26_dead[SB26_DEAD_MAX];
static unsigned sb26_dead_n;
static pthread_mutex_t sb26_dead_lock = PTHREAD_MUTEX_INITIALIZER;

static xpc_object_t
sb26_dead_cached(const char *name)
{
    if (!name) return NULL;
    xpc_object_t reply = NULL;
    pthread_mutex_lock(&sb26_dead_lock);
    for (unsigned i = 0; i < sb26_dead_n; i++) {
        if (!strcmp(sb26_dead[i].name, name) && sb26_dead[i].err) {
            sb26_dead[i].hits++;
            reply = xpc_retain(sb26_dead[i].err);
            break;
        }
    }
    pthread_mutex_unlock(&sb26_dead_lock);
    return reply;
}

static void
sb26_dead_note(const char *name, xpc_object_t reply)
{
    if (!name) return;
    int is_error = reply && xpc_get_type(reply) == XPC_TYPE_ERROR;
    pthread_mutex_lock(&sb26_dead_lock);
    unsigned i;
    for (i = 0; i < sb26_dead_n; i++)
        if (!strcmp(sb26_dead[i].name, name)) break;
    if (i == sb26_dead_n) {
        if (sb26_dead_n == SB26_DEAD_MAX) { pthread_mutex_unlock(&sb26_dead_lock); return; }
        i = sb26_dead_n++;
        memset(&sb26_dead[i], 0, sizeof(sb26_dead[i]));
        snprintf(sb26_dead[i].name, sizeof(sb26_dead[i].name), "%s", name);
    }
    if (!is_error) {
        sb26_dead[i].streak = 0;
        sb26_dead[i].last = NULL;
        if (sb26_dead[i].err) {
            xpc_release(sb26_dead[i].err);
            sb26_dead[i].err = NULL;
            fprintf(stderr, "sb26 DEAD name=%s revived\n", name);
        }
    } else if (!sb26_dead[i].err) {
        sb26_dead[i].streak = (reply == sb26_dead[i].last) ? sb26_dead[i].streak + 1 : 1;
        sb26_dead[i].last = reply;
        if (sb26_dead[i].streak >= SB26_DEAD_STREAK) {
            sb26_dead[i].err = xpc_retain(reply);
            fprintf(stderr, "sb26 DEAD name=%s reply=%p after %u identical errors\n",
                name, reply, sb26_dead[i].streak);
        }
    }
    pthread_mutex_unlock(&sb26_dead_lock);
}

static atomic_uint sb26_xpc_sync_n;
static _Thread_local unsigned sb26_xpc_depth;

static xpc_object_t
sb26_hook_xpc_sync(xpc_connection_t connection, xpc_object_t message)
{
    const char *name = connection ? xpc_connection_get_name(connection) : NULL;
    unsigned n = atomic_fetch_add(&sb26_xpc_sync_n, 1) + 1;
    int is_icons = name && !strcmp(name, "com.apple.iconservices");
    xpc_object_t reply;

    /* v64's first-60 cap missed the requests that matter; the agent dies on
     * iconservices traffic, so every iconservices message is dumped in full. */
    if (n <= 60 || is_icons) {
        char *desc = message ? xpc_copy_description(message) : NULL;
        fprintf(stderr, "sb26 xpc sync #%u -> name=%s msg=%.600s\n",
            n, name ? name : "(anonymous)", desc ? desc : "-");
        if (desc) free(desc);
    } else {
        fprintf(stderr, "sb26 xpc sync #%u -> name=%s\n", n, name ? name : "(anonymous)");
    }

    /* Only reachable if dyld ever resolves the original back to this interposer. */
    if (sb26_xpc_depth) {
        fprintf(stderr, "sb26 xpc sync #%u RECURSION name=%s\n", n, name ? name : "-");
        return NULL;
    }

    /* v84: a service proven dead three times in a row needs no launchd lookup.
     * iconservices keeps its own reply-replay path above. */
    if (!is_icons) {
        xpc_object_t dead = sb26_dead_cached(name);
        if (dead) {
            fprintf(stderr, "sb26 xpc sync #%u DEADCACHE name=%s reply=%p us=0\n",
                n, name ? name : "-", dead);
            return dead;
        }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* v66: the agent dies right after receiving an ISGeneration request and its
     * death strands every in-flight xpc_connection_send_message_with_reply_sync
     * FOREVER (the tap-an-icon freeze). The original call has no timeout, so for
     * iconservices only, the same reply is awaited through the async API under a
     * dispatch semaphore; every other service keeps the plain synchronous call.
     *
     * v70: the 4 s cap itself killed SpringBoard. v69's log shows dozens of its
     * threads in "semaphore_timedwait_trap -> sb26_hook_xpc_sync" and backboardd's
     * system-shell watchdog firing on the shell: with the agent dead, every icon
     * request cost a full 4 s and the main thread never answered. So: a request
     * whose key is already cached returns at once (a tap re-asks for icons it
     * already drew), and a cold one waits 300 ms — a live agent answers in tens of
     * milliseconds, and no UI thread is ever held longer than a few frames. */
    if (is_icons) {
        char key[72];
        sb26_xpc_cache_key(message, key, sizeof(key));
        xpc_object_t hit = sb26_xpc_cache_get(key);

        if (hit) {
            reply = hit;
            fprintf(stderr, "sb26 xpc sync #%u CACHEHIT name=%s key=%.24s\n", n, name, key);
            goto done_icons;
        }

        struct sb26_xpc_reply_wait *wait = sb26_xpc_reply_wait_create();
        if (!wait) {
            int cached = 0;
            reply = sb26_xpc_cache_fallback(key, &cached);
            fprintf(stderr, "sb26 xpc sync #%u PENDINGLIMIT name=%s key=%.24s cached=%d\n",
                n, name, key, cached);
            goto done_icons;
        }
        xpc_connection_send_message_with_reply(connection, message,
            dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
            ^(xpc_object_t rep) {
                /* v68: an XPC_TYPE_ERROR reply is exactly what the sync API returns
                 * when the peer dies mid-call. v67 kept only dictionaries, so that
                 * error became NULL and NSXPCConnection aborted SpringBoard ("root
                 * object is not an XPC dictionary", 12 times in one boot). Keep every
                 * non-NULL reply, errors included; the cache still stores dictionaries
                 * only. The array "key" must stay out of the block (clang rejects
                 * capturing one). */
                sb26_xpc_reply_wait_complete(wait, rep);
            });
        if (dispatch_semaphore_wait(wait->done,
                dispatch_time(DISPATCH_TIME_NOW, 300LL * NSEC_PER_MSEC)) == 0) {
            reply = sb26_xpc_reply_wait_take(wait);
            if (reply) {
                sb26_xpc_cache_put(key, reply);
            } else {
                /* v68 saw the handler itself called with NULL; returning that aborted
                 * SpringBoard. Fall back to a reply that is known to decode. */
                int cached = 0;
                reply = sb26_xpc_cache_fallback(key, &cached);
                fprintf(stderr, "sb26 xpc sync #%u NULLREPLY name=%s key=%.24s cached=%d\n",
                    n, name, key, cached);
            }
        } else {
            /* NSXPCConnection requires a dictionary: NULL aborts SpringBoard with
             * "root object is not an XPC dictionary" (v66). Replay a real reply of
             * the same icon request when one was ever captured; if this key is cold,
             * replay the newest captured one (wrong icon beats a dead SpringBoard). */
            int cached = 0;
            reply = sb26_xpc_cache_fallback(key, &cached);
            fprintf(stderr, "sb26 xpc sync #%u TIMEOUT name=%s key=%.24s cached=%d\n",
                n, name, key, cached);
        }
        sb26_xpc_reply_wait_release(wait);
done_icons:
        ;
    } else {
        sb26_xpc_depth++;
        reply = xpc_connection_send_message_with_reply_sync(connection, message);
        sb26_xpc_depth--;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long us = (long long)(t1.tv_sec - t0.tv_sec) * 1000000LL +
        (t1.tv_nsec - t0.tv_nsec) / 1000;
    if (!is_icons) sb26_dead_note(name, reply);
    fprintf(stderr, "sb26 xpc sync #%u <- name=%s reply=%p us=%lld\n",
        n, name ? name : "-", reply, us);
    return reply;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement;
    const void *original;
} sb26_interposers[] = {
    { (const void *)sb26_hook_MGCopyAnswer, (const void *)MGCopyAnswer },
    { (const void *)sb26_hook_CFBundleCopyBundleLocalizations, (const void *)CFBundleCopyBundleLocalizations },
    { (const void *)sb26_hook_xpc_sync, (const void *)xpc_connection_send_message_with_reply_sync },
};

#include "inject_sb26_airdrop.inc"
#include "inject_sb26_uninstall.inc"

static void
sb26_trigger_poweroff(id self, SEL _cmd, id arg1, id arg2)
{
    (void)self;
    (void)_cmd;
    (void)arg1;
    (void)arg2;
    int one = 1;
    fprintf(stderr, "sb26: poweroff requested via %s, syncing and halting\n",
        _cmd ? sel_getName(_cmd) : "unknown");
    sync();
    sysctlbyname("kern.redwood.halt", NULL, NULL, &one, sizeof(one));
}

static void
install_powerdown_hooks(void)
{
    static const struct {
        const char *cls;
        const char *sel;
    } targets[] = {
        { "SBPowerDownViewController", "powerDownViewRequestPowerDown:" },
        { "SBMainWorkspace", "powerDownViewControllerRequestsPowerDown:" },
        { "SBRestartManager", "shutdownForReason:" },
        { "SBRestartManager", "shutdownWithContext:" },
        { "FBSystemService", "shutdownWithOptions:" },
        { "FBSystemService", "shutdownWithOptions:origin:" },
    };
    unsigned patched = 0;
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        Class cls = objc_getClass(targets[i].cls);
        if (!cls) continue;
        Method m = class_getInstanceMethod(cls, sel_registerName(targets[i].sel));
        if (!m) continue;
        method_setImplementation(m, (IMP)sb26_trigger_poweroff);
        patched++;
    }
    fprintf(stderr, "sb26: powerdown hooks installed (%u methods)\n", patched);
}

/* --- v57: put a real CGImage into every SBIconImageView --------------------
 * v56 proved that nothing but SBCalendarIconImageProvider implements
 * -iconLayerWithInfo:..., so the ICRIconLayer fallback cannot reach the home
 * screen icons. 18 SBIconImageView layers have contents=none while their
 * labels have CGImages; the app bundles carry AppIcon60x60@2x.png. Read that
 * PNG and set it on the view's own layer. Every access is guarded, and the
 * object graph is printed once so a failed lookup tells us what 23G90 answers.
 */
static unsigned sb26_icons_seen, sb26_icons_filled, sb26_icons_refreshed,
                sb26_icons_no_path, sb26_icons_no_image;
static int sb26_icon_diag_done;
/* SBIconImageView objects are recycled: a view that showed Weather can be moved under the Mail
 * label. Remember which bundle supplied the artwork and keep the decoded UIImage alive, so a
 * relabelled view gets the right picture without decoding the PNG again on every pass. */
static char sb26_icon_key, sb26_icon_image_key;

static id
sb26_ask(id object, const char *name)
{
    SEL sel;
    if (!object) return nil;
    sel = sel_registerName(name);
    if (!class_respondsToSelector(object_getClass(object), sel)) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(object, sel);
}

static const char *
sb26_utf8(id string)
{
    SEL sel;
    if (!string) return NULL;
    sel = sel_registerName("UTF8String");
    if (!class_respondsToSelector(object_getClass(string), sel)) return NULL;
    return ((const char *(*)(id, SEL))objc_msgSend)(string, sel);
}

static id
sb26_uiimage_at(const char *path)
{
    Class ui = objc_getClass("UIImage");
    SEL make = sel_registerName("imageWithContentsOfFile:");
    CFStringRef str;
    id image;
    if (!ui || !class_respondsToSelector(object_getClass((id)ui), make)) return nil;
    str = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
    if (!str) return nil;
    image = ((id (*)(Class, SEL, id))objc_msgSend)(ui, make, (id)str);
    CFRelease(str);
    return image;
}

static id
sb26_cgimage_at(const char *path)
{
    return sb26_ask(sb26_uiimage_at(path), "CGImage");
}

/* Some bundles name their icon differently (Preferences.app uses
 * SettingsApp60x60@2x.png). Fall back to scanning the bundle's root. */
static id
sb26_uiimage_by_scan(const char *root)
{
    DIR *dir = opendir(root);
    struct dirent *entry;
    char sixty[256] = "", anyicon[256] = "", path[PATH_MAX];
    const char *chosen;
    id image;
    if (!dir) return nil;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 5 || len >= sizeof(sixty) || strcmp(name + len - 4, ".png") != 0) continue;
        if (strstr(name, "60x60") && !sixty[0]) snprintf(sixty, sizeof(sixty), "%s", name);
        else if ((strstr(name, "Icon") || strstr(name, "icon")) && !anyicon[0])
            snprintf(anyicon, sizeof(anyicon), "%s", name);
    }
    closedir(dir);
    chosen = sixty[0] ? sixty : (anyicon[0] ? anyicon : NULL);
    if (!chosen) return nil;
    if (snprintf(path, sizeof(path), "%s/%s", root, chosen) >= (int)sizeof(path)) return nil;
    image = sb26_uiimage_at(path);
    if (image) fprintf(stderr, "sb26 iconview scan: using %s\n", chosen);
    return image;
}

/* ---------------------------------------------------------------------------
 * v89: clear-glass home screen (user options A + C).
 *
 * Upstream annibale gets clear icons and a translucent Dock from CoreAnimation's
 * own glass shaders. On warsaw the icon artwork is a plain AppIcon PNG that
 * sb26_fill_one_icon() sets on SBIconImageView's layer, because the native
 * iconservicesagent GPU path still traps, so the icons are flat opaque squares
 * with a baked white highlight rim -- the "cut-out" look. Bake the clear look
 * into the bitmap instead: squircle clip, light/white pixels keep less alpha
 * than saturated ones, faint upper-left sheen. The wallpaper then shows through.
 *
 * Dock / search pill / home-screen widgets are glass groups rendered into an
 * opaque offscreen surface; lower their opacity and give them a translucent
 * white base so the purple-blue wallpaper shows through even before the Mesa
 * 0016 / offscreen-clear fixes reach this device.
 *
 * All tunables live in /redwood/etc/glass.conf on the small root disk, so a
 * rebuild of root26.apfs (no recompile) is enough to retune after a device run.
 * --------------------------------------------------------------------------- */
typedef struct { double x, y, w, h; } sb26_rect;

/* Keep source artwork opaque until partial-alpha composition is verified.
 * Retain the rounded clip; the optional glass experiment must be explicit. */
static float glass_icon_color = 1.0f;
static float glass_icon_white = 1.0f;
static float glass_icon_sheen = 0.0f;
static float glass_dock       = 1.0f;
static float glass_search     = 1.0f;
static float glass_widget     = 1.0f;
static int   glass_on         = 1;
static int   glass_conf_read;

static void
sb26_glass_conf(void)
{
    FILE *f;
    char line[256];
    if (glass_conf_read) return;
    glass_conf_read = 1;
    f = fopen("/redwood/etc/glass.conf", "r");
    if (!f) {
        fprintf(stderr, "sb26 glass: no /redwood/etc/glass.conf, using defaults\n");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        float value;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (sscanf(line, " %63[^= \t] = %f", key, &value) != 2) continue;
        if (!strcmp(key, "icon_color")) glass_icon_color = value;
        else if (!strcmp(key, "icon_white")) glass_icon_white = value;
        else if (!strcmp(key, "icon_sheen")) glass_icon_sheen = value;
        else if (!strcmp(key, "dock")) glass_dock = value;
        else if (!strcmp(key, "search")) glass_search = value;
        else if (!strcmp(key, "widget")) glass_widget = value;
        else if (!strcmp(key, "on")) glass_on = (int)value;
    }
    fclose(f);
    fprintf(stderr,
        "sb26 glass: conf on=%d icon=%.2f/%.2f sheen=%.2f dock=%.2f search=%.2f widget=%.2f\n",
        glass_on, glass_icon_color, glass_icon_white, glass_icon_sheen, glass_dock, glass_search,
        glass_widget);
}

struct sb26_glass_entry { char key[192]; id image; };
static struct sb26_glass_entry glass_cache[64];
static unsigned glass_used, glass_made, glass_hits, glass_failed;

/* The CGImage we hand to CALayer is cached per bundle: sb26_fill_one_icon runs
 * nine times in the first 150 s and on every com.redwood.icons notify. */
static id
sb26_glass_image(const char *key, id src)
{
    CGImageRef cg, out;
    CGColorSpaceRef cs;
    CGContextRef ctx;
    CGPathRef path;
    CGRect box;
    CGFloat radius;
    size_t w, h, stride, x, y;
    unsigned char *px;
    unsigned i;
    float color_a, white_a, sheen_amount;

    if (!glass_on || !src || !key) return src;
    for (i = 0; i < glass_used; i++)
        if (!strncmp(glass_cache[i].key, key, sizeof(glass_cache[i].key) - 1)) {
            glass_hits++;
            return glass_cache[i].image;
        }
    if (glass_used >= sizeof(glass_cache) / sizeof(glass_cache[0])) return src;

    cg = (CGImageRef)(void *)src;
    w = CGImageGetWidth(cg);
    h = CGImageGetHeight(cg);
    if (!w || !h || w > 1024 || h > 1024) { glass_failed++; return src; }
    color_a = glass_icon_color;
    white_a = glass_icon_white;
    if (color_a < 0.05f) color_a = 0.05f;
    if (color_a > 1.0f) color_a = 1.0f;
    if (white_a < 0.0f) white_a = 0.0f;
    if (white_a > color_a) white_a = color_a;
    sheen_amount = glass_icon_sheen;
    if (!(sheen_amount >= 0.0f)) sheen_amount = 0.0f;
    if (sheen_amount > 1.0f) sheen_amount = 1.0f;

    cs = CGColorSpaceCreateDeviceRGB();
    ctx = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) { glass_failed++; return src; }
    box = CGRectMake(0, 0, (CGFloat)w, (CGFloat)h);
    radius = (CGFloat)(w < h ? w : h) * 0.2237;
    path = CGPathCreateWithRoundedRect(box, radius, radius, NULL);
    if (path) {
        CGContextAddPath(ctx, path);
        CGContextClip(ctx);
    }
    /* This is a CGImage -> CGBitmapContext -> CGImage round trip, not a draw
     * into UIKit's flipped context. Both CoreGraphics images use the same
     * convention: another CTM flip inverts the artwork handed to CALayer.
     * Keep the source orientation; the rounded clip is symmetric. */
    CGContextDrawImage(ctx, box, cg);

    px = (unsigned char *)CGBitmapContextGetData(ctx);
    stride = CGBitmapContextGetBytesPerRow(ctx);
    if (px) {
        for (y = 0; y < h; y++) {
            unsigned char *row = px + y * stride;
            for (x = 0; x < w; x++) {
                unsigned char *q = row + x * 4;   /* RGBA, big-endian 32 */
                unsigned a = q[3];
                float r, g, b, mx, mn, sat, lum, white, target, na, sheen;
                if (!a) continue;
                r = (float)q[0] * 255.0f / (float)a;
                g = (float)q[1] * 255.0f / (float)a;
                b = (float)q[2] * 255.0f / (float)a;
                mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
                mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
                sat = mx > 0.0f ? (mx - mn) / mx : 0.0f;
                lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
                white = lum * (1.0f - sat);       /* pale, unsaturated pixels */
                if (white < 0.0f) white = 0.0f;
                if (white > 1.0f) white = 1.0f;
                target = color_a + (white_a - color_a) * white;
                na = ((float)a / 255.0f) * target;
                sheen = sheen_amount * (1.0f - 0.5f * ((float)x / (float)w
                                              + (float)y / (float)h));
                r += (255.0f - r) * sheen;
                g += (255.0f - g) * sheen;
                b += (255.0f - b) * sheen;
                q[0] = (unsigned char)(r * na + 0.5f);
                q[1] = (unsigned char)(g * na + 0.5f);
                q[2] = (unsigned char)(b * na + 0.5f);
                q[3] = (unsigned char)(na * 255.0f + 0.5f);
            }
        }
    }
    out = CGBitmapContextCreateImage(ctx);
    if (path) CGPathRelease(path);
    CGContextRelease(ctx);
    if (!out) { glass_failed++; return src; }
    snprintf(glass_cache[glass_used].key, sizeof(glass_cache[glass_used].key),
        "%s", key);
    glass_cache[glass_used].image = (id)(void *)out;
    glass_used++;
    glass_made++;
    return (id)(void *)out;
}

static sb26_rect
sb26_glass_bounds(id layer)
{
    sb26_rect (*fn)(id, SEL) = (sb26_rect (*)(id, SEL))objc_msgSend;
    return fn(layer, sel_registerName("bounds"));
}

static void
sb26_glass_setf(id layer, const char *name, double value)
{
    ((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName(name), value);
}

static void
sb26_glass_seto(id layer, const char *name, id value)
{
    if (value)
        ((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName(name), value);
}

static id
sb26_glass_cgcolor(double white, double alpha)
{
    Class cls = objc_getClass("UIColor");
    SEL sel;
    id color;
    if (!cls) return nil;
    sel = sel_registerName("colorWithWhite:alpha:");
    color = ((id (*)(Class, SEL, double, double))objc_msgSend)((id)cls, sel,
        white, alpha);
    return sb26_ask(color, "CGColor");
}

static unsigned glass_passes, glass_applied, glass_logs;

static void
sb26_glass_apply(id layer, int kind, const char *cls, sb26_rect b)
{
    Class tx = objc_getClass("CATransaction");
    float opacity;
    double radius;
    const char *tag;

    if (kind == 1) { opacity = glass_dock;   radius = b.h * 0.335; tag = "dock";   }
    else if (kind == 2) { opacity = glass_search; radius = b.h * 0.5; tag = "search"; }
    else { opacity = glass_widget; radius = b.w * 0.17; tag = "widget"; }

    /* v90: on this Metal driver a group opacity below 1 makes the ENTIRE Dock
     * (including its four icons) disappear -- v89 measured 74.7% of the dock
     * band changing, all of it turning into bare wallpaper. Do not touch a
     * glass group at all until the driver composites layer alpha correctly.
     * glass.conf keeps the switches so a fixed driver can enable C without a
     * recompile; 1.0 means "leave the layer exactly as SpringBoard made it". */
    if (opacity >= 0.99f) return;

    if (tx) {
        ((void (*)(Class, SEL))objc_msgSend)(tx, sel_registerName("begin"));
        ((void (*)(Class, SEL, BOOL))objc_msgSend)(tx,
            sel_registerName("setDisableActions:"), YES);
    }
    sb26_glass_setf(layer, "setOpacity:", opacity);
    if (radius > 1.0 && radius < 80.0)
        sb26_glass_setf(layer, "setCornerRadius:", radius);
    sb26_glass_seto(layer, "setBackgroundColor:", sb26_glass_cgcolor(1.0, 0.09));
    sb26_glass_seto(layer, "setBorderColor:", sb26_glass_cgcolor(1.0, 0.22));
    sb26_glass_setf(layer, "setBorderWidth:", 0.8);
    if (tx) ((void (*)(Class, SEL))objc_msgSend)(tx, sel_registerName("commit"));

    glass_applied++;
    if (glass_logs < 40) {
        glass_logs++;
        fprintf(stderr,
            "sb26 glass: %-6s %-34s bounds=%.0fx%.0f opacity=%.2f radius=%.1f\n",
            tag, cls, b.w, b.h, opacity, radius);
    }
}

static void
sb26_glass_walk(id view, int depth, int in_home)
{
    const char *cls;
    id layer = nil, subviews;
    unsigned long count, i;
    sb26_rect b = { 0, 0, 0, 0 };
    int kind = 0, home = in_home;

    if (!view || depth > 16) return;
    cls = class_getName(object_getClass(view));
    if (cls && strstr(cls, "SBHomeScreenWindow")) home = 1;
    layer = sb26_ask(view, "layer");
    if (layer) b = sb26_glass_bounds(layer);
    if (home && cls && layer && glass_on) {
        if (strstr(cls, "SBDockView")) kind = 1;
        else if (strstr(cls, "_GlassGroupView") && b.w > 250.0 && b.h > 60.0
                 && b.h < 140.0) kind = 1;
        else if (strstr(cls, "SBHSearchPillView")) kind = 2;
        else if (strstr(cls, "SBHWidgetStackScrollView")) kind = 3;
        else if (strstr(cls, "NSKVONotifying_UIView") && b.w > 140.0
                 && b.w < 200.0 && b.h > 140.0 && b.h < 200.0) kind = 3;
        if (kind) sb26_glass_apply(layer, kind, cls, b);
    }
    subviews = sb26_ask(view, "subviews");
    if (!subviews) return;
    if (!class_respondsToSelector(object_getClass(subviews),
        sel_registerName("count"))) return;
    count = ((unsigned long (*)(id, SEL))objc_msgSend)(subviews,
        sel_registerName("count"));
    for (i = 0; i < count; i++) {
        id child = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subviews,
            sel_registerName("objectAtIndex:"), i);
        sb26_glass_walk(child, depth + 1, home);
    }
}

static void
sb26_glass_pass(void)
{
    Class wc = objc_getClass("UIWindow");
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows = nil, app;
    unsigned long count, i;

    if (wc && class_respondsToSelector(object_getClass((id)wc), all))
        windows = ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)wc, all, YES, YES);
    if (!windows) {
        app = sb26_ask((id)objc_getClass("UIApplication"), "sharedApplication");
        windows = sb26_ask(app, "windows");
    }
    if (!windows) return;
    sb26_glass_conf();
    glass_passes++;
    count = ((unsigned long (*)(id, SEL))objc_msgSend)(windows,
        sel_registerName("count"));
    for (i = 0; i < count; i++) {
        id w = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows,
            sel_registerName("objectAtIndex:"), i);
        sb26_glass_walk(w, 0, 0);
    }
    if (glass_passes <= 3 || glass_passes % 10 == 0)
        fprintf(stderr, "sb26 glass: pass=%u applied=%u made=%u hits=%u failed=%u\n",
            glass_passes, glass_applied, glass_made, glass_hits, glass_failed);
}

static void
sb26_fill_one_icon(id view)
{
    static const char *names[] = {
        "AppIcon60x60@2x.png", "AppIcon60x60@3x.png", "AppIcon76x76@2x.png",
        "SettingsApp60x60@2x.png", "Icon-60@2x.png", "Icon@2x.png", "Icon.png",
        "AppIcon60x60@2x~iphone.png", "icon-60@2x.png",
    };
    id layer, icon, app, base, url, prev, cached, image = nil, cg = nil;
    const char *root, *prev_root;
    char path[PATH_MAX];
    unsigned i;
    Class tx;

    sb26_icons_seen++;
    layer = sb26_ask(view, "layer");
    if (!layer) return;

    icon = sb26_ask(view, "icon");
    app = sb26_ask(icon, "application");
    if (!app) app = sb26_ask(icon, "applicationProxy");
    base = sb26_ask(app, "bundlePath");
    if (!base) base = sb26_ask(app, "path");
    if (!base) { url = sb26_ask(app, "bundleURL"); base = sb26_ask(url, "path"); }
    root = sb26_utf8(base);

    if (!sb26_icon_diag_done) {
        static const char *probes[] = {
            "application", "applicationProxy", "bundleIdentifier", "bundlePath", "path",
            "displayName", "nodeIdentifier", "uniqueIdentifier", "iconIdentifier",
            "applicationBundleIdentifier", "applicationBundleID", "iconImage", "image",
            "icon", "applicationDisplayName", "applicationID",
        };
        sb26_icon_diag_done = 1;
        fprintf(stderr, "sb26 iconview: view=%s icon=%s app=%s root=%s layer=%s\n",
            class_getName(object_getClass(view)), icon ? class_getName(object_getClass(icon)) : "-",
            app ? class_getName(object_getClass(app)) : "-", root ? root : "-",
            class_getName(object_getClass(layer)));
        for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            id value = sb26_ask(icon, probes[i]);
            const char *text;
            if (!value) value = sb26_ask(view, probes[i]);
            if (!value) continue;
            text = sb26_utf8(value);
            fprintf(stderr, "sb26 iconview probe %-30s = %s %s\n", probes[i],
                class_getName(object_getClass(value)), text ? text : "(object)");
        }
    }
    if (!root) {
        /* 23G90 may answer a bundle identifier but no path; NSBundle can map it. */
        id bid = sb26_ask(icon, "bundleIdentifier");
        Class nb = objc_getClass("NSBundle");
        SEL with = sel_registerName("bundleWithIdentifier:");
        if (!bid) bid = sb26_ask(icon, "applicationBundleIdentifier");
        if (bid && nb && class_respondsToSelector(object_getClass((id)nb), with)) {
            id bundle = ((id (*)(Class, SEL, id))objc_msgSend)(nb, with, bid);
            root = sb26_utf8(sb26_ask(bundle, "bundlePath"));
        }
    }
    if (!root) { sb26_icons_no_path++; return; }

    /* The labels and the artwork still disagree after v61 bound a bundle path to each view.
     * Print the traversal order together with what the view says its icon is, so a mismatch
     * between the view's model and the label can be told from a wrong PNG. */
    if (sb26_icons_seen <= 24) {
        id bid = sb26_ask(icon, "applicationBundleID");
        const char *bundle = sb26_utf8(bid);
        sb26_pt pos = { -1, -1 };
        if (class_respondsToSelector(object_getClass(layer), sel_registerName("position"))) {
            sb26_pt (*at)(id, SEL) = (sb26_pt (*)(id, SEL))objc_msgSend;
            pos = at(layer, sel_registerName("position"));
        }
        id parent = sb26_ask(view, "superview");
        id picon = sb26_ask(parent, "icon");
        id papp = sb26_ask(picon, "application");
        const char *proot = sb26_utf8(sb26_ask(papp, "bundlePath"));
        sb26_pt ppos = { -1, -1 };
        id player = sb26_ask(parent, "layer");
        if (player && class_respondsToSelector(object_getClass(player), sel_registerName("position"))) {
            sb26_pt (*at)(id, SEL) = (sb26_pt (*)(id, SEL))objc_msgSend;
            ppos = at(player, sel_registerName("position"));
        }
        fprintf(stderr, "sb26 iconmap: #%u root=%s bid=%s pos=%.0f,%.0f"
            " | parent=%s proot=%s ppos=%.0f,%.0f\n",
            sb26_icons_seen, root, bundle ? bundle : "-", pos.x, pos.y,
            parent ? class_getName(object_getClass(parent)) : "-", proot ? proot : "-",
            ppos.x, ppos.y);
    }

    /* v59/v60 only asked "is contents non-nil?", so a recycled view kept its old artwork under
     * the new label. Compare the bundle path the cached picture came from with the view's CURRENT
     * icon. Even when they match, re-assert our CGImage: -updateImage: can replace layer.contents
     * with the (nil / black) Metal-produced image behind our back. */
    prev = objc_getAssociatedObject(view, &sb26_icon_key);
    cached = objc_getAssociatedObject(view, &sb26_icon_image_key);
    prev_root = sb26_utf8(prev);
    if (prev_root && cached && strcmp(prev_root, root) == 0) {
        cg = sb26_ask(cached, "CGImage");
        if (cg) sb26_icons_refreshed++;
        else cached = nil;
    }
    if (!cg) {
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (snprintf(path, sizeof(path), "%s/%s", root, names[i]) >= (int)sizeof(path)) continue;
            if (access(path, R_OK) != 0) continue;
            image = sb26_uiimage_at(path);
            if (image) break;
        }
        if (!image) image = sb26_uiimage_by_scan(root);
        if (!image) { sb26_icons_no_image++; return; }
        cg = sb26_ask(image, "CGImage");
        if (!cg) { sb26_icons_no_image++; return; }
        {
            CFStringRef bound = CFStringCreateWithCString(kCFAllocatorDefault, root,
                kCFStringEncodingUTF8);
            if (bound) {
                objc_setAssociatedObject(view, &sb26_icon_key, (id)bound,
                    OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                CFRelease(bound);
                objc_setAssociatedObject(view, &sb26_icon_image_key, image,
                    OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
        }
        sb26_icons_filled++;
    }

    tx = objc_getClass("CATransaction");
    if (tx) {
        ((void (*)(Class, SEL))objc_msgSend)(tx, sel_registerName("begin"));
        ((void (*)(Class, SEL, BOOL))objc_msgSend)(tx, sel_registerName("setDisableActions:"), YES);
    }
    sb26_glass_conf();
    ((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setContents:"),
        sb26_glass_image(root, cg));
    ((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setContentsGravity:"), (id)CFSTR("resizeAspect"));
    /* Deliberately NO layer opacity here. v91 tried 0.72 on SBIconImageView and
     * every PNG-backed icon vanished (only the natively rendered Clock survived,
     * as a translucent purple glass tile). Until the driver composites layer
     * alpha, the squircle clip above is the only safe clear-glass change. */
    if (tx) ((void (*)(Class, SEL))objc_msgSend)(tx, sel_registerName("commit"));
}

static void
sb26_fill_icons_recursive(id view, int depth)
{
    id subviews;
    unsigned long count, i;
    const char *cls;
    if (!view || depth > 14) return;
    cls = class_getName(object_getClass(view));
    if (cls && strstr(cls, "SBIconImageView")) sb26_fill_one_icon(view);
    subviews = sb26_ask(view, "subviews");
    if (!subviews) return;
    if (!class_respondsToSelector(object_getClass(subviews), sel_registerName("count"))) return;
    count = ((unsigned long (*)(id, SEL))objc_msgSend)(subviews, sel_registerName("count"));
    for (i = 0; i < count; i++) {
        id child = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subviews,
            sel_registerName("objectAtIndex:"), i);
        sb26_fill_icons_recursive(child, depth + 1);
    }
}

static void
sb26_fill_all_icons(void)
{
    Class wc = objc_getClass("UIWindow");
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows = nil, app;
    unsigned long count, i;
    if (wc && class_respondsToSelector(object_getClass((id)wc), all))
        windows = ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)wc, all, YES, YES);
    if (!windows) {
        app = sb26_ask((id)objc_getClass("UIApplication"), "sharedApplication");
        windows = sb26_ask(app, "windows");
    }
    if (!windows) return;
    count = ((unsigned long (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count"));
    for (i = 0; i < count; i++) {
        id w = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows,
            sel_registerName("objectAtIndex:"), i);
        sb26_fill_icons_recursive(w, 0);
    }
}

/* PosterBoard / MercuryPosterExtension never starts on this port: the wallpaper window is a
 * _SBWallpaperSecureWindow whose UIWindowLayer has bg=000000ff and contents=none. Paint the PNG
 * that tools/mkwallpaper.py put on the small root disk into that window's layer. Same fallback
 * idea as the icons: set the contents ourselves instead of waiting for the poster pipeline. */
static unsigned sb26_wall_seen, sb26_wall_filled, sb26_wall_had, sb26_wall_noimg,
                sb26_wall_layers;

/* v59 set the PNG on the window layer only: the layer dump then showed
 *   layer 0 UIWindowLayer ... contents=CGImage 1280x2772
 *   layer 1 CALayer view=UIView ... bg=000000ff masks=1 contents=none
 * i.e. an opaque black full-screen child covered the image completely. Walk the wallpaper
 * window's view tree and give every empty layer the same image. CoverSheet's dimming and
 * dest-out overlays are left alone (they have their own compositing filters). */
static void
sb26_fill_wall_subviews(id view, id cg, int depth)
{
    const char *cls = view ? class_getName(object_getClass(view)) : NULL;
    id subviews, layer;
    unsigned long count, i;

    if (!view || depth > 6 || (cls && strstr(cls, "CoverSheet"))) return;
    layer = sb26_ask(view, "layer");
    if (layer && !sb26_ask(layer, "contents")) {
        ((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setContents:"), cg);
        ((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setContentsGravity:"),
            (id)CFSTR("resizeAspectFill"));
        sb26_wall_layers++;
    }
    subviews = sb26_ask(view, "subviews");
    if (!subviews) return;
    count = ((unsigned long (*)(id, SEL))objc_msgSend)(subviews, sel_registerName("count"));
    for (i = 0; i < count; i++) {
        id child = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subviews,
            sel_registerName("objectAtIndex:"), i);
        sb26_fill_wall_subviews(child, cg, depth + 1);
    }
}

static void
sb26_fill_wallpaper(void)
{
    Class wc = objc_getClass("UIWindow");
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows = nil, app, cg = nil;
    unsigned long count, i;

    if (wc && class_respondsToSelector(object_getClass((id)wc), all))
        windows = ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)wc, all, YES, YES);
    if (!windows) {
        app = sb26_ask((id)objc_getClass("UIApplication"), "sharedApplication");
        windows = sb26_ask(app, "windows");
    }
    if (!windows) return;
    count = ((unsigned long (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count"));
    for (i = 0; i < count; i++) {
        id w = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows,
            sel_registerName("objectAtIndex:"), i);
        const char *cls = w ? class_getName(object_getClass(w)) : NULL;
        unsigned before;
        if (!cls || !strstr(cls, "Wallpaper")) continue;
        sb26_wall_seen++;
        if (!cg) cg = sb26_cgimage_at("/redwood/share/wallpaper.png");
        if (!cg) { sb26_wall_noimg++; continue; }
        before = sb26_wall_layers;
        sb26_fill_wall_subviews(w, cg, 0);
        if (sb26_wall_layers > before) sb26_wall_filled++;
        else sb26_wall_had++;
    }
}

/* Icon provider classes are loaded lazily. Re-scan a few times during the first minute,
 * then ask SpringBoard to rebuild the icon views so the newly hooked providers are used. */
static void
sb26_icon_late_step(void *context)
{
    uintptr_t at = (uintptr_t)context;
    sb26_install_all_icon_layer_fallbacks();
    sb26_fill_all_icons();
    sb26_fill_wallpaper();
    sb26_glass_pass();
    fprintf(stderr,
        "sb26: icons +%lus: filled=%u seen=%u refilled=%u nopath=%u noimg=%u | hooked=%u calls=%u wrapped=%u"
        " | wall seen=%u filled=%u had=%u noimg=%u layers=%u\n",
        (unsigned long)at, sb26_icons_filled, sb26_icons_seen, sb26_icons_refreshed,
        sb26_icons_no_path, sb26_icons_no_image, sb26_icon_hooked,
        sb26_icon_fallback_calls, sb26_icon_fallback_wrapped,
        sb26_wall_seen, sb26_wall_filled, sb26_wall_had, sb26_wall_noimg, sb26_wall_layers);
}

/* Host-side "oem sh notify com.redwood.icons" re-runs the icon fill at any time, so a
 * relayout/scroll can be checked without waiting for the next scheduled pass. */
static void
sb26_install_icon_refresh_notify(void)
{
    int token;
    notify_register_dispatch("com.redwood.icons", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        /* Host-side retuning: edit /redwood/etc/glass.conf, then post this
         * notify; drop the baked bitmaps so the new alpha values take effect. */
        glass_conf_read = 0;
        glass_used = 0;
        sb26_glass_conf();
        sb26_fill_all_icons();
        sb26_glass_pass();
        fprintf(stderr, "sb26: icon refresh notify: filled=%u refilled=%u seen=%u\n",
            sb26_icons_filled, sb26_icons_refreshed, sb26_icons_seen);
    });
}

static void
discovery_image_callback(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    Class airdrop = objc_getClass("SFAirDropDiscoveryController");
    if (airdrop) install_airdrop_hooks(airdrop);

    Class session = objc_getClass("AVOutputDeviceDiscoverySession");
    if (session) install_screen_mirroring_hooks(session);

    Class mru = objc_getClass("MRUMirroringController");
    if (mru) install_mru_mirroring_hooks(mru);

    Class cal = objc_getClass("SBCalendarIconImageProvider");
    if (cal) install_calendar_icon_hooks(cal);

    install_uninstall_hooks();
}

__attribute__((constructor)) static void
install_sb26(void)
{
    const char *prog = getprogname();

    sb26_install_crash_logger();

    if (!prog || (strcmp(prog, "SpringBoard") && strcmp(prog, "Preferences"))) return;

    sb26_ensure_chinese_preferences();

    if (!is_build_23g90()) {
        fprintf(stderr, "sb26: skipped build mismatch or unreadable SystemVersion.plist\n");
        return;
    }

    if (strcmp(prog, "SpringBoard") != 0) return;

    size_t size = 0;
    uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&size);
    if (base < 0x180000000UL || !size) {
        fprintf(stderr, "sb26: skipped invalid shared cache range\n");
        return;
    }
    uintptr_t slide = base - 0x180000000UL;
    static const unsigned char expected_bytes[16] = {
        0x08,0x30,0x40,0x39,0xe8,0x07,0x00,0x37,0x7f,0x23,0x03,0xd5,0xff,0x43,0x01,0xd1
    };
    Method method = checked_method("SBLockdownManager", "_setupActivationState", "v16@0:8",
        0x22452fdc0UL + slide, expected_bytes);
    if (method) {
        method_setImplementation(method, (IMP)setup_activation_state);
        int installed = code_address(method_getImplementation(method)) == code_address((IMP)setup_activation_state);
        fprintf(stderr, "sb26: build=23G90 activation_installed=%d\n", installed);
    }
    install_bitmap_selection(slide);
    install_unlock();
    sb26_install_all_icon_layer_fallbacks();
    {
        static const int icon_delays[] = { 3, 8, 18, 30, 45, 60, 90, 120, 150 };
        for (unsigned i = 0; i < sizeof(icon_delays) / sizeof(icon_delays[0]); i++)
            dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)icon_delays[i] * NSEC_PER_SEC),
                dispatch_get_main_queue(), (void *)(uintptr_t)icon_delays[i], sb26_icon_late_step);
    }
    sb26_install_layer_dump();
    sb26_install_icon_refresh_notify();
    sb26_install_rate(slide);
    install_powerdown_hooks();

    _dyld_register_func_for_add_image(haptic_image_callback);
    Class engine_cls = objc_getClass("CHHapticEngine");
    if (engine_cls) {
        install_haptic_hooks(engine_cls);
    }

    _dyld_register_func_for_add_image(discovery_image_callback);
    Class airdrop_cls = objc_getClass("SFAirDropDiscoveryController");
    if (airdrop_cls) install_airdrop_hooks(airdrop_cls);
    Class session_cls = objc_getClass("AVOutputDeviceDiscoverySession");
    if (session_cls) install_screen_mirroring_hooks(session_cls);
    Class mru_cls = objc_getClass("MRUMirroringController");
    if (mru_cls) install_mru_mirroring_hooks(mru_cls);
    Class cal_cls = objc_getClass("SBCalendarIconImageProvider");
    if (cal_cls) install_calendar_icon_hooks(cal_cls);

    install_uninstall_hooks();
}
