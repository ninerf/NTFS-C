// ntfs — tiny read/write NTFS mounter for macOS.
// Swaps macOS's read-only NTFS mount for an ntfs-3g (macFUSE) read-write one.
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef FS_KIND
#define FS_KIND "ntfs" // DiskArbitration volume kind to manage (override only for testing)
#endif
#define MAXVOLS 64

extern char **environ;

enum { UNMOUNTED, READONLY, READWRITE };
static const char *state_str[] = {"not mounted", "read-only", "read-write"};

typedef struct {
    char bsd[32], name[256], mnt[MAXPATHLEN];
    long long size;
    int state;
} vol_t;

static DASessionRef session;
static char self[MAXPATHLEN];
static int force;

static void cfstr(CFTypeRef s, char *buf, size_t n) {
    buf[0] = 0;
    if (s && CFGetTypeID(s) == CFStringGetTypeID())
        CFStringGetCString(s, buf, n, kCFStringEncodingUTF8);
}

// Fill state/mnt from the live mount table (prefers a read-write entry).
static void mount_state(vol_t *v) {
    struct statfs *m;
    int n = getmntinfo(&m, MNT_NOWAIT);
    char dev[48];
    snprintf(dev, sizeof dev, "/dev/%s", v->bsd);
    v->state = UNMOUNTED;
    v->mnt[0] = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(m[i].f_mntfromname, dev) && strcmp(m[i].f_mntfromname, v->bsd)) continue;
        int st = (m[i].f_flags & MNT_RDONLY) ? READONLY : READWRITE;
        if (st > v->state) {
            v->state = st;
            strlcpy(v->mnt, m[i].f_mntonname, sizeof v->mnt);
        }
    }
}

// Returns 1 and fills *v if the disk holds an NTFS volume.
static int probe(DADiskRef d, vol_t *v) {
    CFDictionaryRef desc = DADiskCopyDescription(d);
    if (!desc) return 0;
    char kind[32];
    cfstr(CFDictionaryGetValue(desc, kDADiskDescriptionVolumeKindKey), kind, sizeof kind);
    int ok = !strcmp(kind, FS_KIND) && DADiskGetBSDName(d);
    if (ok) {
        memset(v, 0, sizeof *v);
        strlcpy(v->bsd, DADiskGetBSDName(d), sizeof v->bsd);
        cfstr(CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey), v->name, sizeof v->name);
        CFNumberRef sz = CFDictionaryGetValue(desc, kDADiskDescriptionMediaSizeKey);
        if (sz) CFNumberGetValue(sz, kCFNumberLongLongType, &v->size);
        mount_state(v);
    }
    CFRelease(desc);
    return ok;
}

static int cmp_vol(const void *a, const void *b) {
    const vol_t *x = a, *y = b;
    int dx = atoi(x->bsd + 4), dy = atoi(y->bsd + 4);
    return dx != dy ? dx - dy : strcmp(x->bsd, y->bsd);
}

static int scan(vol_t *out) {
    DIR *dir = opendir("/dev");
    struct dirent *e;
    int n = 0;
    while (dir && (e = readdir(dir)) && n < MAXVOLS) {
        if (strncmp(e->d_name, "disk", 4) || !isdigit((unsigned char)e->d_name[4])) continue;
        DADiskRef d = DADiskCreateFromBSDName(NULL, session, e->d_name);
        if (!d) continue;
        if (probe(d, &out[n])) n++;
        CFRelease(d);
    }
    if (dir) closedir(dir);
    qsort(out, n, sizeof *out, cmp_vol);
    return n;
}

// --- DiskArbitration mount/unmount (synchronous via the run loop) ---

static int da_status;
static void da_done(DADiskRef d, DADissenterRef dis, void *ctx) {
    da_status = dis ? DADissenterGetStatus(dis) : 0;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static int da_op(const char *bsd, int mount) {
    DADiskRef d = DADiskCreateFromBSDName(NULL, session, bsd);
    if (!d) return -1;
    if (mount) DADiskMount(d, NULL, kDADiskMountOptionDefault, da_done, NULL);
    else DADiskUnmount(d, kDADiskUnmountOptionDefault, da_done, NULL);
    CFRunLoopRun();
    CFRelease(d);
    return da_status;
}

// --- helpers ---

static const char *find_ntfs3g(void) {
    static const char *paths[] = {
        "/opt/homebrew/bin/ntfs-3g", "/usr/local/bin/ntfs-3g",
        "/opt/homebrew/opt/ntfs-3g-mac/bin/ntfs-3g", "/usr/local/opt/ntfs-3g-mac/bin/ntfs-3g",
        "/opt/homebrew/sbin/ntfs-3g", "/usr/local/sbin/ntfs-3g", NULL};
    for (int i = 0; paths[i]; i++)
        if (access(paths[i], X_OK) == 0) return paths[i];
    return NULL;
}

static int have_macfuse(void) { return access("/Library/Filesystems/macfuse.fs", F_OK) == 0; }

static void human(long long b, char *out, size_t n) {
    const char *u = "BKMGTP";
    double x = b;
    while (x >= 1024 && u[1]) x /= 1024, u++;
    snprintf(out, n, *u == 'B' ? "%.0f%c" : "%.1f%c", x, *u);
}

// Owner for files on the mount: the sudo caller, else the logged-in console user.
static void owner(uid_t *uid, gid_t *gid) {
    struct stat st;
    const char *u = getenv("SUDO_UID"), *g = getenv("SUDO_GID");
    if (u && g) { *uid = atoi(u); *gid = atoi(g); }
    else if (stat("/dev/console", &st) == 0 && st.st_uid) { *uid = st.st_uid; *gid = st.st_gid; }
    else { *uid = getuid(); *gid = getgid(); }
}

// Create a free /Volumes/<name>[ N] directory.
static int make_mountpoint(const vol_t *v, char *out) {
    char base[256];
    strlcpy(base, v->name[0] ? v->name : v->bsd, sizeof base);
    for (char *p = base; *p; p++) if (*p == '/') *p = '_';
    if (base[0] == '.') base[0] = '_';
    for (int i = 0; i < 100; i++) {
        if (i) snprintf(out, MAXPATHLEN, "/Volumes/%s %d", base, i);
        else snprintf(out, MAXPATHLEN, "/Volumes/%s", base);
        // rmdir succeeds only on an empty, non-mounted leftover dir.
        if ((rmdir(out) == 0 || errno == ENOENT) && mkdir(out, 0755) == 0) return 0;
    }
    return -1;
}

// Escape ',' and '\' for a FUSE -o value.
static void opt_escape(const char *s, char *out, size_t n) {
    size_t j = 0;
    for (; *s && j + 2 < n; s++) {
        if (*s == ',' || *s == '\\') out[j++] = '\\';
        out[j++] = *s;
    }
    out[j] = 0;
}

static void hint(int rc, const char *bsd) {
    switch (rc) {
    case 14:
        fprintf(stderr, "  Windows is hibernated or Fast Startup is on. Boot Windows and fully shut down\n"
                        "  (hold Shift while clicking Shut Down) or disable Fast Startup.\n"
                        "  Or run `ntfs mount -f %s` to delete the hibernation file (loses the\n"
                        "  saved Windows session).\n", bsd);
        break;
    case 15:
        fprintf(stderr, "  The volume was not cleanly unmounted. Run chkdsk on it in Windows, or\n"
                        "  `sudo ntfsfix /dev/%s`.\n", bsd);
        break;
    case 21:
        fprintf(stderr, "  macFUSE could not load. Allow its system extension in System Settings >\n"
                        "  Privacy & Security, then reboot. See `ntfs doctor`.\n");
        break;
    case 124:
        fprintf(stderr, "  ntfs-3g timed out.\n");
        break;
    }
}

// --- commands ---

static int do_mount(vol_t *v) {
    if (v->state == READWRITE) {
        printf("%s (%s) is already read-write at %s\n", v->bsd, v->name, v->mnt);
        return 0;
    }
    const char *bin = find_ntfs3g();
    if (!bin || !have_macfuse()) {
        fprintf(stderr, "ntfs-3g or macFUSE is missing; run `ntfs doctor`\n");
        return 1;
    }
    if (v->state == READONLY && da_op(v->bsd, 0) != 0) {
        fprintf(stderr, "%s: could not unmount the read-only volume (files in use?)\n", v->bsd);
        return 1;
    }

    char mnt[MAXPATHLEN], dev[48], name[512], opts[1024];
    if (make_mountpoint(v, mnt)) {
        fprintf(stderr, "%s: could not create a mount point in /Volumes\n", v->bsd);
        da_op(v->bsd, 1);
        return 1;
    }
    uid_t uid; gid_t gid;
    owner(&uid, &gid);
    opt_escape(v->name[0] ? v->name : v->bsd, name, sizeof name);
    snprintf(dev, sizeof dev, "/dev/%s", v->bsd);
    snprintf(opts, sizeof opts,
             "local,allow_other,auto_xattr,windows_names,noatime,uid=%d,gid=%d,volname=%s%s",
             uid, gid, name, force ? ",remove_hiberfile" : "");

    char *argv[] = {(char *)bin, dev, mnt, "-o", opts, NULL};
    pid_t pid, r = 0;
    int status = 0, rc;
    if (posix_spawn(&pid, bin, NULL, NULL, argv, environ) != 0) {
        perror("ntfs-3g");
        rc = 1;
    } else {
        for (int i = 0; i < 300 && (r = waitpid(pid, &status, WNOHANG)) == 0; i++) usleep(100000);
        if (r != pid) { kill(pid, SIGKILL); waitpid(pid, &status, 0); rc = 124; }
        else rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    if (rc == 0) {
        printf("%s (%s) mounted read-write at %s\n", v->bsd, v->name, mnt);
        return 0;
    }
    fprintf(stderr, "%s: ntfs-3g failed (exit %d)\n", v->bsd, rc);
    hint(rc, v->bsd);
    rmdir(mnt);
    if (da_op(v->bsd, 1) == 0) fprintf(stderr, "  Restored the read-only mount.\n");
    return 1;
}

static int do_unmount(vol_t *v) {
    if (v->state == UNMOUNTED) {
        printf("%s is not mounted\n", v->bsd);
        return 0;
    }
    if (v->state == READONLY) {
        if (da_op(v->bsd, 0) == 0) { printf("%s unmounted\n", v->bsd); return 0; }
        fprintf(stderr, "%s: unmount failed (files in use?)\n", v->bsd);
        return 1;
    }
    if (unmount(v->mnt, force ? MNT_FORCE : 0) != 0) {
        fprintf(stderr, "%s: %s%s\n", v->bsd, strerror(errno),
                errno == EBUSY ? " (close open files, or use -f)" : "");
        return 1;
    }
    rmdir(v->mnt);
    printf("%s unmounted; safe to unplug\n", v->bsd);
    return 0;
}

static int cmd_list(void) {
    vol_t v[MAXVOLS];
    int n = scan(v);
    if (!n) { printf("No NTFS volumes found.\n"); return 0; }
    printf("%-10s %-24s %8s  %-11s  %s\n", "DEVICE", "NAME", "SIZE", "STATE", "MOUNTED AT");
    for (int i = 0; i < n; i++) {
        char sz[16];
        human(v[i].size, sz, sizeof sz);
        printf("%-10s %-24.24s %8s  %-11s  %s\n", v[i].bsd, v[i].name, sz, state_str[v[i].state], v[i].mnt);
    }
    return 0;
}

// Run fn on the target: a device/name, or "all" volumes whose read-write state matches want_rw.
static int each(const char *target, int (*fn)(vol_t *), int want_rw) {
    vol_t v[MAXVOLS];
    int n = scan(v), hits = 0, fails = 0, all = !target || !strcmp(target, "all");
    if (target && !strncmp(target, "/dev/", 5)) target += 5;
    for (int i = 0; i < n; i++) {
        if (all ? (v[i].state == READWRITE) != want_rw
                : (strcmp(v[i].bsd, target) && strcmp(v[i].name, target)))
            continue;
        hits++;
        fails += fn(&v[i]) != 0;
    }
    if (!hits) {
        if (all) printf("Nothing to do.\n");
        else { fprintf(stderr, "No NTFS volume named '%s'. Try `ntfs list`.\n", target); return 1; }
    }
    return fails ? 1 : 0;
}

// --- watch: remount NTFS drives read-write as they appear ---

static char tried[MAXVOLS][32];

static int was_tried(const char *bsd, int add) {
    for (int i = 0; i < MAXVOLS; i++) if (!strcmp(tried[i], bsd)) return 1;
    if (add) for (int i = 0; i < MAXVOLS; i++) if (!tried[i][0]) { strlcpy(tried[i], bsd, 32); break; }
    return 0;
}

static void on_disk(DADiskRef d, void *ctx) {
    vol_t v;
    if (!probe(d, &v) || v.state != READONLY || was_tried(v.bsd, 1)) return;
    printf("%s (%s) appeared read-only, remounting read-write\n", v.bsd, v.name);
    fflush(stdout);
    char *argv[] = {self, "mount", v.bsd, NULL};
    pid_t pid;
    posix_spawn(&pid, self, NULL, NULL, argv, environ);
}

static void on_change(DADiskRef d, CFArrayRef keys, void *ctx) { on_disk(d, ctx); }

static void on_gone(DADiskRef d, void *ctx) {
    const char *bsd = DADiskGetBSDName(d);
    for (int i = 0; bsd && i < MAXVOLS; i++) if (!strcmp(tried[i], bsd)) tried[i][0] = 0;
}

static int cmd_watch(void) {
    signal(SIGCHLD, SIG_IGN); // auto-reap spawned mounts
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(match, kDADiskDescriptionVolumeKindKey, CFSTR(FS_KIND));
    const void *k[] = {kDADiskDescriptionVolumePathKey};
    CFArrayRef keys = CFArrayCreate(NULL, k, 1, &kCFTypeArrayCallBacks);
    DARegisterDiskAppearedCallback(session, match, on_disk, NULL);
    DARegisterDiskDescriptionChangedCallback(session, match, keys, on_change, NULL);
    DARegisterDiskDisappearedCallback(session, match, on_gone, NULL);
    printf("Watching for NTFS drives (Ctrl-C to stop)...\n");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}

static int cmd_doctor(void) {
    const char *bin = find_ntfs3g();
    int fuse = have_macfuse();
    printf("macFUSE : %s\n", fuse ? "installed" : "MISSING");
    printf("ntfs-3g : %s\n", bin ? bin : "MISSING");
    if (!fuse || !bin) {
        printf("\nInstall with Homebrew:\n");
        if (!fuse) printf("  brew install --cask macfuse\n");
        if (!bin) printf("  brew install gromgit/fuse/ntfs-3g-mac\n");
    }
    printf("\nmacFUSE uses a kernel extension. On Apple Silicon, allow it once:\n"
           "  1. Shut down, hold the power button, open Options (Recovery).\n"
           "  2. Utilities > Startup Security Utility > Reduced Security >\n"
           "     check \"Allow user management of kernel extensions\".\n"
           "  3. Restart, run `ntfs mount`, then click Allow in System Settings >\n"
           "     Privacy & Security, and restart again.\n");
    return fuse && bin ? 0 : 1;
}

static void usage(void) {
    printf("ntfs: read/write NTFS drives on macOS\n\n"
           "  ntfs [list]               show NTFS volumes\n"
           "  ntfs mount [disk|all]     remount read-write (default: all)\n"
           "  ntfs unmount <disk|all>   unmount a read-write volume\n"
           "  ntfs watch                auto-mount NTFS drives read-write as they appear\n"
           "  ntfs doctor               check and explain dependencies\n\n"
           "  -f   mount: delete the Windows hibernation file; unmount: force\n"
           "  <disk> is a device (disk4s1) or a volume name.\n");
}

static void ensure_root(int argc, char **argv) {
    if (geteuid() == 0) return;
    char *args[argc + 2];
    args[0] = "sudo";
    args[1] = self;
    for (int i = 1; i < argc; i++) args[i + 1] = argv[i];
    args[argc + 1] = NULL;
    execv("/usr/bin/sudo", args);
    perror("sudo");
    exit(1);
}

int main(int argc, char **argv) {
    char raw[MAXPATHLEN];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0 || !realpath(raw, self)) strlcpy(self, argv[0], sizeof self);
    signal(SIGCHLD, SIG_DFL); // may be inherited as ignored from `watch`

    const char *cmd = NULL, *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) cmd = "help";
        else if (!cmd) cmd = argv[i];
        else if (!target) target = argv[i];
    }
    if (!cmd) cmd = "list";

    int is_mount = !strcmp(cmd, "mount");
    int is_unmount = !strcmp(cmd, "unmount") || !strcmp(cmd, "umount");
    int is_watch = !strcmp(cmd, "watch");
    if (!strcmp(cmd, "help")) { usage(); return 0; }
    if (!strcmp(cmd, "doctor")) return cmd_doctor();
    if (strcmp(cmd, "list") && !is_mount && !is_unmount && !is_watch) { usage(); return 1; }
    if (is_unmount && !target) { fprintf(stderr, "usage: ntfs unmount <disk|all>\n"); return 1; }
    if (is_mount || is_unmount || is_watch) ensure_root(argc, argv);

    session = DASessionCreate(NULL);
    DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    if (is_watch) return cmd_watch();
    if (is_mount) return each(target, do_mount, 0);
    if (is_unmount) return each(target, do_unmount, 1);
    return cmd_list();
}
