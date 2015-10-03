// pa-jail.cc -- Peteramati program sets up a jail for student code
// Peteramati is Copyright (c) 2013-2015 Eddie Kohler and others
// Distributed under an MIT-like license; see LICENSE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <termios.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <utime.h>
#include <assert.h>
#include <getopt.h>
#include <fnmatch.h>
#include <string>
#include <map>
#include <iostream>
#include <sys/ioctl.h>
#if __linux__
#include <mntent.h>
#include <sched.h>
#elif __APPLE__
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#endif

#define ROOT 0

#define FLAG_CP 1        // copy even if source is symlink
#define FLAG_NOLINK 2    // never link from source

#ifndef O_PATH
#define O_PATH 0
#endif

static std::map<std::string, int> dst_table;
static std::multimap<std::string, std::string> lnk_table;
static int exit_value = 0;
static bool verbose = false;
static bool dryrun = false;
static bool copy_samedev = false;
static bool foreground = false;
static bool quiet = false;
static FILE* verbosefile = stdout;
static std::string linkdir;
static std::map<std::string, int> linkdir_dirtable;
static std::string dstroot;
static std::string pidfilename;
static int pidfd = -1;
static std::map<std::string, int> umount_table;
static volatile sig_atomic_t got_sigterm = 0;
static int sigpipe[2];

enum jailaction {
    do_start, do_init, do_run, do_rm, do_mv
};


// pathname helpers

static std::string path_endslash(const std::string& path) {
    if (path.empty() || path.back() != '/')
        return path + "/";
    else
        return path;
}

static std::string path_noendslash(std::string path) {
    while (path.length() > 1 && path.back() == '/')
        path = path.substr(0, path.length() - 1);
    return path;
}

static std::string path_parentdir(const std::string& path) {
    size_t npos = path.length();
    while (npos > 0 && path[npos - 1] == '/')
        --npos;
    while (npos > 0 && path[npos - 1] != '/')
        --npos;
    return path.substr(0, npos);
}

static std::string shell_quote(const std::string& argument) {
    std::string quoted;
    size_t last = 0;
    for (size_t pos = 0; pos != argument.length(); ++pos)
        if ((pos == 0 && argument[pos] == '~')
            || !(isalnum((unsigned char) argument[pos])
                 || argument[pos] == '_'
                 || argument[pos] == '-'
                 || argument[pos] == '~'
                 || argument[pos] == '.'
                 || argument[pos] == '/')) {
            if (quoted.empty())
                quoted = "'";
            if (argument[pos] == '\'') {
                quoted += argument.substr(last, pos - last) + "'\\''";
                last = pos + 1;
            }
        }
    if (quoted.empty())
        return argument;
    else {
        quoted += argument.substr(last) + "'";
        return quoted;
    }
}


static int perror_fail(const char* format, const char* arg1) {
    fprintf(stderr, format, arg1, strerror(errno));
    exit_value = 1;
    return 1;
}

static const char* uid_to_name(uid_t u) {
    static uid_t old_uid = -1;
    static char buf[128];
    if (u != old_uid) {
        old_uid = u;
        if (struct passwd *pw = getpwuid(u))
            snprintf(buf, sizeof(buf), "%s", pw->pw_name);
        else
            snprintf(buf, sizeof(buf), "%u", (unsigned) u);
    }
    return buf;
}

static const char* gid_to_name(gid_t g) {
    static gid_t old_gid = -1;
    static char buf[128];
    if (g != old_gid) {
        old_gid = g;
        if (struct group *gr = getgrgid(g))
            snprintf(buf, sizeof(buf), "%s", gr->gr_name);
        else
            snprintf(buf, sizeof(buf), "%u", (unsigned) g);
    }
    return buf;
}


static int v_mkdir(const char* pathname, mode_t mode) {
    if (verbose)
        fprintf(verbosefile, "mkdir -m 0%o %s\n", mode, pathname);
    return dryrun ? 0 : mkdir(pathname, mode);
}

static int v_mkdirat(int dirfd, const char* component, mode_t mode, const std::string& pathname) {
    if (verbose)
        fprintf(verbosefile, "mkdir -m 0%o %s\n", mode, pathname.c_str());
    return dryrun ? 0 : mkdirat(dirfd, component, mode);
}

static int v_fchmod(int fd, mode_t mode, const std::string& pathname) {
    if (verbose)
        fprintf(verbosefile, "chmod 0%o %s\n", mode, pathname.c_str());
    return dryrun ? 0 : fchmod(fd, mode);
}

static int v_ensuredir(const char* pathname, mode_t mode) {
    struct stat s;
    int r = stat(pathname, &s);
    if (r == 0 && S_ISDIR(s.st_mode))
        return 0;
    else if (r != 0 && errno == ENOENT) {
        int r = v_mkdir(pathname, mode);
        return r ? r : 1;
    } else {
        if (r == 0)
            errno = ENOTDIR;
        return -1;
    }
}

static bool x_link_eexist_ok(const char* oldpath, const char* newpath) {
    // Maybe the file is already linked.
    struct stat oldstat, newstat;
    if (stat(oldpath, &oldstat) == 0 && stat(newpath, &newstat) == 0
        && oldstat.st_dev == newstat.st_dev
        && oldstat.st_ino == newstat.st_ino)
        return true;

    // Maybe we are trying to link a file using two pathnames, where
    // an intermediate directory was a symbolic link.
    std::string dst(newpath);
    size_t npos = dst.length() + 1, slash;
    while (npos != 0
           && (slash = dst.rfind('/', npos - 1)) != std::string::npos) {
        std::string dstdir = dst.substr(0, slash);
        for (auto it = lnk_table.lower_bound(dstdir);
             it != lnk_table.end() && it->first == dstdir;
             ++it) {
            std::string lnkdst = it->second + dst.substr(slash);
            if (dst_table.find(lnkdst) != dst_table.end())
                return true;
        }
        npos = slash;
    }
    return false;
}

static int x_link(const char* oldpath, const char* newpath) {
    if (verbose)
        fprintf(verbosefile, "ln %s %s\n", oldpath, newpath);
    if (!dryrun && link(oldpath, newpath) != 0
        && (errno != EEXIST || !x_link_eexist_ok(oldpath, newpath)))
        return -1;
    return 0;
}

static int x_chmod(const char* path, mode_t mode) {
    if (verbose)
        fprintf(verbosefile, "chmod 0%o %s\n", mode, path);
    if (!dryrun && chmod(path, mode) != 0)
        return perror_fail("chmod %s: %s\n", path);
    return 0;
}

static int x_lchown(const char* path, uid_t owner, gid_t group) {
    if (verbose)
        fprintf(verbosefile, "chown -h %s:%s %s\n", uid_to_name(owner), gid_to_name(group), path);
    if (!dryrun && lchown(path, owner, group) != 0)
        return perror_fail("chown %s: %s\n", path);
    return 0;
}

static int x_lchownat(int fd, const char* component, uid_t owner, gid_t group, const std::string& dirpath) {
    if (verbose)
        fprintf(verbosefile, "chown -h %s:%s %s%s\n", uid_to_name(owner), gid_to_name(group), dirpath.c_str(), component);
    if (!dryrun && fchownat(fd, component, owner, group, AT_SYMLINK_NOFOLLOW) != 0)
        return perror_fail("chown %s: %s\n", (dirpath + component).c_str());
    return 0;
}

static int x_fchown(int fd, uid_t owner, gid_t group, const std::string& path) {
    if (verbose)
        fprintf(verbosefile, "chown -h %s:%s %s\n", uid_to_name(owner), gid_to_name(group), path.c_str());
    if (!dryrun && fchown(fd, owner, group) != 0)
        return perror_fail("chown %s: %s\n", path.c_str());
    return 0;
}

static bool x_mknod_eexist_ok(const char* path, mode_t mode, dev_t dev) {
    struct stat st;
    if (stat(path, &st) == 0 && st.st_mode == mode && st.st_rdev == dev)
        return true;
    return false;
}

static const char* dev_name(mode_t m, dev_t d) {
    static char buf[128];
    if (S_ISCHR(m))
        snprintf(buf, sizeof(buf), "c %d %d", major(d), minor(d));
    else if (S_ISBLK(m))
        snprintf(buf, sizeof(buf), "b %d %d", major(d), minor(d));
    else if (S_ISFIFO(m))
        return "p";
    else
        snprintf(buf, sizeof(buf), "%u %u", (unsigned) m, (unsigned) d);
    return buf;
}

static int x_mknod(const char* path, mode_t mode, dev_t dev) {
    if (verbose)
        fprintf(verbosefile, "mknod -m 0%o %s %s\n", mode, path, dev_name(mode, dev));
    if (!dryrun && mknod(path, mode, dev) != 0
        && (errno != EEXIST || !x_mknod_eexist_ok(path, mode, dev)))
        return perror_fail("mknod %s: %s\n", path);
    return 0;
}

static bool x_symlink_eexist_ok(const char* oldpath, const char* newpath) {
    char lnkbuf[4096];
    int old_errno = errno;
    ssize_t r = readlink(newpath, lnkbuf, sizeof(lnkbuf));
    bool answer = (size_t) r == (size_t) strlen(oldpath) && memcmp(lnkbuf, oldpath, r) == 0;
    errno = old_errno;
    return answer;
}

static int x_symlink(const char* oldpath, const char* newpath) {
    if (verbose)
        fprintf(verbosefile, "ln -s %s %s\n", oldpath, newpath);
    if (!dryrun && symlink(oldpath, newpath) != 0
        && (errno != EEXIST || !x_symlink_eexist_ok(oldpath, newpath)))
        return perror_fail("symlink %s: %s\n", (std::string(oldpath) + " " + newpath).c_str());
    return 0;
}

static int x_waitpid(pid_t child, int flags) {
    int status;
    while (1) {
        pid_t w = waitpid(child, &status, flags);
        if (w == child && WIFEXITED(status))
            return WEXITSTATUS(status);
        else if (w == child)
            return 128 + WTERMSIG(status);
        else if (w == 0) {
            errno = EAGAIN;
            return -1;
        } else if (w == -1 && errno != EINTR)
            return -1;
    }
}


static __attribute__((noreturn)) void perror_exit(const char* message) {
    fprintf(stderr, "%s: %s\n", message, strerror(errno));
    exit(1);
}


// jailmaking

struct mountslot {
    std::string fsname;
    std::string type;
    std::string alloptions;
    unsigned long opts;
    std::string data;
    bool allowed;
};
typedef std::map<std::string, mountslot> mount_table_type;
mount_table_type mount_table;

static bool allow_mount(const char* dest, const mountslot& ms) {
    return ((strcmp(dest, "/proc") == 0 && ms.type == "proc")
            || (strcmp(dest, "/sys") == 0 && ms.type == "sysfs")
            || (strcmp(dest, "/dev") == 0 && ms.type == "udev")
            || (strcmp(dest, "/dev/pts") == 0 && ms.type == "devpts"));
}

#if __linux__
#define MFLAG(x) MS_ ## x
#elif __APPLE__
#define MFLAG(x) MNT_ ## x
#endif

struct mountarg {
    const char *name;
    int value;
};
static const mountarg mountargs[] = {
    { ",nosuid,", MFLAG(NOSUID) },
    { ",nodev,", MFLAG(NODEV) },
    { ",noexec,", MFLAG(NOEXEC) },
    { ",ro,", MFLAG(RDONLY) },
    { ",rw,", 0 },
#if __linux__
    { ",noatime,", MS_NOATIME },
    { ",nodiratime,", MS_NODIRATIME },
#ifdef MS_RELATIME
    { ",relatime,", MS_RELATIME },
#endif
#ifdef MS_STRICTATIME
    { ",strictatime,", MS_STRICTATIME },
#endif
#endif
    { NULL, 0 }
};

static int populate_mount_table() {
    static bool mount_table_populated = false;
    if (mount_table_populated)
        return 0;
    mount_table_populated = true;
#if __linux__
    FILE* f = setmntent("/proc/mounts", "r");
    if (!f)
        return perror_fail("open %s: %s\n", "/proc/mounts");
    while (struct mntent* me = getmntent(f)) {
        char options[BUFSIZ], *options_pos;
        snprintf(options, sizeof(options), ",%s,", me->mnt_opts);
        unsigned long opts = 0;
        for (const mountarg *ma = mountargs; ma->name; ++ma)
            if ((options_pos = strstr(options, ma->name))) {
                opts |= ma->value;
                char* post = options_pos + strlen(ma->name) - 1;
                memmove(options_pos, post, strlen(post) + 1);
            }
        int l;
        while ((l = strlen(options)) > 1 && options[l - 1] == ',')
            options[l - 1] = '\0';
        mountslot ms{me->mnt_fsname, me->mnt_type, me->mnt_opts,
                opts, &options[1], false};
        ms.allowed = allow_mount(me->mnt_dir, ms);
        mount_table[me->mnt_dir] = ms;
    }
    fclose(f);
    return 0;
#elif __APPLE__
    struct statfs* mntbuf;
    int nmntbuf = getmntinfo(&mntbuf, MNT_NOWAIT);
    for (struct statfs* me = mntbuf; me != mntbuf + nmntbuf; ++me) {
        mountslot ms{me->f_mntfromname, me->f_fstypename, std::string(),
                me->f_flags, std::string(), false};
        for (const mountarg* ma = mountargs; ma->name; ++ma)
            if (ma->value & me->f_flags)
                ms.alloptions += (ms.alloptions.empty() ? "" : ",")
                    + std::string(&ma->name[1], strlen(ma->name) - 2);
        ms.allowed = allow_mount(me->f_mntonname, ms);
        mount_table[me->f_mntonname] = ms;
    }
    return 0;
#endif
}

#if __APPLE__
int mount(const char*, const char* target, const char* fstype,
          unsigned long flags, const void*) {
    return ::mount(fstype, target, flags, NULL);
}

int umount(const char* dir) {
    return ::unmount(dir, 0);
}
#endif

static int handle_mount(const mountslot& ms, std::string dst) {
    auto it = mount_table.find(dst);
    if (it != mount_table.end()
        && it->second.fsname == ms.fsname
        && it->second.type == ms.type
        && it->second.opts == ms.opts
        && it->second.data == ms.data)
        // already mounted
        return 0;
    if (verbose)
        fprintf(verbosefile, "mount -i -n -t %s%s%s %s %s\n",
                ms.type.c_str(), ms.alloptions.empty() ? "" : " -o ",
                ms.alloptions.c_str(), ms.fsname.c_str(), dst.c_str());
    if (!dryrun && mount(ms.fsname.c_str(), dst.c_str(), ms.type.c_str(),
                         ms.opts,
                         ms.data.empty() ? NULL : ms.data.c_str()) != 0)
        return perror_fail("mount %s: %s\n", dst.c_str());
    return 0;
}

static int handle_umount(const mount_table_type::iterator& it) {
    if (verbose)
        fprintf(verbosefile, "umount -i -n %s\n", it->first.c_str());
    if (!dryrun && umount(it->first.c_str()) != 0) {
        fprintf(stderr, "umount %s: %s\n", it->first.c_str(), strerror(errno));
        exit(1);
    }
    if (dryrun)
        umount_table[it->first.c_str()] = 1;
    return 0;
}

static int handle_copy(const std::string& src, const std::string& dst,
                       bool check_parents, int flags,
                       dev_t jaildev, mode_t* srcmode);

static void handle_symlink_dst(std::string src, std::string dst,
                               std::string lnk, dev_t jaildev)
{
    std::string dst_lnkin = dst;

    // expand `lnk` into `dst`
    if (lnk[0] == '/') {
        src = lnk;
        dst = dstroot + lnk;
    } else {
        while (1) {
            if (src.length() == 1) {
            give_up:
                return;
            }
            size_t srcslash = src.rfind('/', src.length() - 2),
                dstslash = dst.rfind('/', dst.length() - 2);
            if (srcslash == std::string::npos || dstslash == std::string::npos
                || dstslash < dstroot.length())
                goto give_up;
            src = src.substr(0, srcslash + 1);
            dst = dst.substr(0, dstslash + 1);
            if (lnk.length() > 3 && lnk[0] == '.' && lnk[1] == '.'
                && lnk[2] == '/')
                lnk = lnk.substr(3);
            else
                break;
        }
        src += lnk;
        dst += lnk;
    }

    if (dst.substr(dstroot.length(), 6) != "/proc/") {
        mode_t srcmode;
        int r = handle_copy(src, dst, true, 0, jaildev, &srcmode);
        // remember directory-level symbolic links
        if (r == 0 && S_ISDIR(srcmode)) {
            lnk_table.insert(std::make_pair(dst, dst_lnkin));
            lnk_table.insert(std::make_pair(dst_lnkin, dst));
        }
    }
}

static int x_cp_p(const std::string& src, const std::string& dst) {
    if (verbose)
        fprintf(verbosefile, "cp -p %s %s\n", src.c_str(), dst.c_str());
    if (dryrun)
        return 0;

    pid_t child = fork();
    if (child == 0) {
        const char* args[6] = {
            "/bin/cp", "-p", src.c_str(), dst.c_str(), NULL
        };
        execv("/bin/cp", (char**) args);
        exit(1);
    } else if (child < 0)
        return perror_fail("%s: %s\n", "fork");

    int status = x_waitpid(child, 0);
    if (status == 0)
        return 0;
    else if (status != -1)
        return perror_fail("/bin/cp %s: Bad exit status\n", dst.c_str());
    else
        return perror_fail("/bin/cp %s: Did not exit\n", dst.c_str());
}

static int copy_for_xdev_link(const std::string& src, const std::string& lnk) {
    // create superdirectories
    size_t pos = linkdir.length() - 1;
    while ((pos = lnk.find('/', pos + 1)) != std::string::npos) {
        std::string lnksuper = lnk.substr(0, pos);
        if (linkdir_dirtable.find(lnksuper) == linkdir_dirtable.end()) {
            struct stat dst;
            if (lstat(lnksuper.c_str(), &dst) != 0) {
                if (errno != ENOENT)
                    return perror_fail("lstat %s: %s\n", lnksuper.c_str());
                if (v_mkdir(lnksuper.c_str(), 0770) != 0 && errno != EEXIST)
                    return perror_fail("mkdir %s: %s\n", lnksuper.c_str());
            } else if (!S_ISDIR(dst.st_mode))
                return perror_fail("lstat %s: Not a directory\n", lnksuper.c_str());
            linkdir_dirtable[lnksuper] = 1;
        }
    }

    // run /bin/cp -p
    return x_cp_p(src, lnk);
}

static int handle_xdev_link(const std::string& src, const std::string& dst,
                            const struct stat& st) {
    struct stat lst;
    std::string lnk = linkdir + src;

    int r = lstat(lnk.c_str(), &lst);
    if (r != 0
        || lst.st_mode != st.st_mode
        || lst.st_uid != st.st_uid
        || lst.st_gid != st.st_gid
        || lst.st_size != st.st_size
        || lst.st_mtime != st.st_mtime) {
        if (r == 0 && S_ISDIR(lst.st_mode))
            return perror_fail("%s: Is a directory\n", lnk.c_str());
        if (copy_for_xdev_link(src, lnk))
            return 1;
    }

    if (x_link(lnk.c_str(), dst.c_str()) != 0)
        return perror_fail("link %s: %s\n", (dst + " " + lnk).c_str());
    return 0;
}

static int handle_copy(const std::string& src, const std::string& dst,
                       bool check_parents, int flags,
                       dev_t jaildev, mode_t* srcmode) {
    if (dst_table.find(dst) != dst_table.end())
        return 1;
    dst_table[dst] = 1;

    struct stat ss, ds;

    if (check_parents) {
        size_t last_slash = dst.rfind('/');
        if (last_slash != 0
            && last_slash != std::string::npos
            && last_slash != dst.length() - 1) {
            size_t last_nchars = dst.length() - last_slash;
            if (src.length() > last_nchars
                && src.substr(src.length() - last_nchars) == dst.substr(dst.length() - last_nchars)) {
                std::string dstdir = dst.substr(0, last_slash);
                if (lstat(dstdir.c_str(), &ss) == -1 && errno == ENOENT)
                    handle_copy(src.substr(0, src.length() - last_nchars),
                                dst.substr(0, dst.length() - last_nchars),
                                true, 0, jaildev, NULL);
            }
        }
    }

    if (lstat(src.c_str(), &ss) != 0)
        return perror_fail("lstat %s: %s\n", src.c_str());
    if (srcmode)
        *srcmode = ss.st_mode;
    ds.st_uid = ds.st_gid = ROOT;

    if (S_ISREG(ss.st_mode) && (flags & FLAG_NOLINK)) {
        if (x_cp_p(src, dst) != 0)
            return 1;
        ds = ss;
    } else if (S_ISREG(ss.st_mode) && !copy_samedev && !(flags & FLAG_CP)
               && ss.st_dev == jaildev) {
        if (x_link(src.c_str(), dst.c_str()) != 0)
            return perror_fail("link %s: %s\n", (dst + " " + src).c_str());
        ds = ss;
    } else if (S_ISREG(ss.st_mode)
               || (S_ISLNK(ss.st_mode) && (flags & FLAG_CP))) {
        errno = EXDEV;
        if (linkdir.empty() || handle_xdev_link(src, dst, ss) != 0)
            return perror_fail("link %s: %s\n", dst.c_str());
        ds = ss;
    } else if (S_ISDIR(ss.st_mode)) {
        // allow setuid/setgid bits
        // allow the presence of a different directory
        mode_t perm = ss.st_mode & (S_ISUID | S_ISGID | S_IRWXU | S_IRWXG | S_IRWXO);
        if (v_mkdir(dst.c_str(), perm) == 0)
            ds.st_mode = perm | S_IFDIR;
        else if (lstat(dst.c_str(), &ds) != 0)
            return perror_fail("lstat %s: %s\n", dst.c_str());
        else if (!S_ISDIR(ds.st_mode))
            return perror_fail("lstat %s: Not a directory\n", dst.c_str());
    } else if (S_ISCHR(ss.st_mode) || S_ISBLK(ss.st_mode)) {
        ss.st_mode &= (S_IFREG | S_IFCHR | S_IFBLK | S_IFIFO | S_IFSOCK | S_ISUID | S_ISGID | S_IRWXU | S_IRWXG | S_IRWXO);
        if (!dryrun && x_mknod(dst.c_str(), ss.st_mode, ss.st_rdev) != 0)
            return 1;
        ds.st_mode = ss.st_mode;
    } else if (S_ISLNK(ss.st_mode)) {
        char lnkbuf[4096];
        ssize_t r = readlink(src.c_str(), lnkbuf, sizeof(lnkbuf));
        if (r == -1)
            return perror_fail("readlink %s: %s\n", src.c_str());
        else if (r == sizeof(lnkbuf))
            return perror_fail("%s: Symbolic link too long\n", src.c_str());
        lnkbuf[r] = 0;
        if (x_symlink(lnkbuf, dst.c_str()) != 0)
            return 1;
        ds.st_mode = ss.st_mode;
        handle_symlink_dst(src, dst, std::string(lnkbuf), jaildev);
    } else
        return perror_fail("%s: Odd file type\n", src.c_str());

    // XXX preserve sticky bits/setuid/setgid?
    if (ds.st_mode != ss.st_mode
        && x_chmod(dst.c_str(), ss.st_mode))
        return 1;
    if ((ds.st_uid != ss.st_uid || ds.st_gid != ss.st_gid)
        && x_lchown(dst.c_str(), ss.st_uid, ss.st_gid))
        return 1;

    if (S_ISDIR(ss.st_mode)) {
        auto it = mount_table.find(src.c_str());
        if (it != mount_table.end() && it->second.allowed)
            return handle_mount(it->second, dst);
    }

    return 0;
}

static int construct_jail(dev_t jaildev, FILE* f) {
    // prepare root
    if (x_chmod(dstroot.c_str(), 0755)
        || x_lchown(dstroot.c_str(), 0, 0))
        return 1;
    dst_table[dstroot + "/"] = 1;

    // Mounts
    populate_mount_table();
#if __linux__
    {
        std::string proc("/proc");
        handle_copy(proc, dstroot + proc, true, 0, jaildev, NULL);
        std::string devpts("/dev/pts");
        handle_copy(devpts, dstroot + devpts, true, 0, jaildev, NULL);
        std::string devptmx("/dev/ptmx");
        handle_copy(devptmx, dstroot + devptmx, true, 0, jaildev, NULL);
    }
#endif

    // Read a line at a time
    std::string cursrcdir("/"), curdstdir(dstroot);
    int base_flags = linkdir.empty() ? FLAG_NOLINK : 0;

    char buf[BUFSIZ];
    while (fgets(buf, BUFSIZ, f)) {
        int l = strlen(buf);
        while (l > 0 && isspace((unsigned char) buf[l-1]))
            buf[--l] = 0;
        if (l == 0 || buf[0] == '#')
            continue;

        // 'directory:'
        if (buf[l - 1] == ':') {
            if (l == 2 && buf[0] == '.')
                cursrcdir = std::string("/");
            else if (l > 2 && buf[0] == '.' && buf[1] == '/')
                cursrcdir = std::string(buf + 1, buf + l - 1);
            else
                cursrcdir = std::string(buf, buf + l - 1);
            if (cursrcdir[0] != '/')
                cursrcdir = std::string("/") + cursrcdir;
            while (cursrcdir.length() > 1 && cursrcdir[cursrcdir.length() - 1] == '/' && cursrcdir[cursrcdir.length() - 2] == '/')
                cursrcdir = cursrcdir.substr(0, cursrcdir.length() - 1);
            if (cursrcdir[cursrcdir.length() - 1] != '/')
                cursrcdir += '/';
            curdstdir = dstroot + cursrcdir;
            continue;
        }

        // '[FLAGS]'
        int flags = base_flags;
        if (buf[l - 1] == ']') {
            for (--l; l > 0 && buf[l-1] != '['; --l)
                /* do nothing */;
            if (l == 0)
                continue;
            char* p;
            if ((p = strstr(&buf[l], "cp"))
                && (p[-1] == '[' || p[-1] == ',')
                && (p[2] == ']' || p[2] == ','))
                flags |= FLAG_CP;
            do {
                buf[--l] = 0;
            } while (l > 0 && isspace((unsigned char) buf[l-1]));
        }

        std::string src, dst;
        char* arrow = strstr(buf, " <- ");
        if (buf[0] == '/' && arrow) {
            src = std::string(arrow + 4);
            dst = curdstdir + std::string(buf, arrow);
        } else if (buf[0] == '/') {
            src = std::string(buf);
            dst = curdstdir + std::string(buf, buf + l);
        } else if (arrow) {
            src = std::string(arrow + 4, buf + l);
            dst = curdstdir + std::string(buf, arrow);
        } else {
            src = cursrcdir + std::string(buf, buf + l);
            dst = curdstdir + std::string(buf, buf + l);
        }
        handle_copy(src, dst, buf[0] == '/', flags, jaildev, NULL);
    }

    return exit_value;
}


// main program

static std::string check_filename(std::string name) {
    const char *allowed_chars = "/0123456789-._ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz~";
    char buf[1024];

    if (strspn(name.c_str(), allowed_chars) != name.length()
        || name.empty()
        || name[0] == '~'
        || name.length() >= sizeof(buf))
        return std::string();

    char* out = buf;
    for (const char* s = name.c_str(); *s; ++s) {
        *out++ = *s;
        if (*s == '.' && (s[1] == '/' || s[1] == '\0')
            && s != name.c_str() && s[-1] == '/') {
            --out;
            ++s;
        } else if (*s == '.' && s[1] == '.' && (s[2] == '/' || s[2] == '\0')
                   && (s == name.c_str() || s[-1] == '/'))
            return std::string();
        while (*s == '/' && s[1] == '/')
            ++s;
    }
    while (out > buf + 1 && out[-1] == '/')
        --out;
    *out = '\0';
    return std::string(buf, out - buf);
}

static std::string absolute(const std::string& dir) {
    if (!dir.empty() && dir[0] == '/')
        return dir;
    FILE* p = popen("pwd", "r");
    char buf[BUFSIZ], crap[1];
    size_t buflen;
    if (fgets(buf, BUFSIZ, p)
        && (!fgets(crap, 1, p) || crap[0] == '\0')
        && (buflen = strnlen(buf, BUFSIZ)) < BUFSIZ) {
        pclose(p);
        while (isspace((unsigned char) buf[buflen - 1]))
            buf[--buflen] = '\0';
        while (buflen > 0 && buf[buflen - 1] == '/')
            buf[--buflen] = '\0';
        return std::string(buf) + std::string("/") + dir;
    } else {
        fprintf(stderr, "pwd: Bogus absolute path\n");
        exit(1);
    }
}

static void x_rm_rf_under(int parentdirfd, std::string component,
                          std::string dirname) {
    dirname = path_endslash(dirname);
    int dirfd = openat(parentdirfd, component.c_str(), O_RDONLY);
    if (dirfd == -1) {
        fprintf(stderr, "%s: %s\n", dirname.c_str(), strerror(errno));
        exit(1);
    }
    DIR* dir = fdopendir(dirfd);
    if (!dir) {
        fprintf(stderr, "%s: %s\n", dirname.c_str(), strerror(errno));
        exit(1);
    }
    while (struct dirent* de = readdir(dir)) {
        // XXX check file system type?
        if (de->d_type == DT_DIR) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            std::string next_component = de->d_name;
            std::string next_dirname = dirname + next_component;
            if (umount_table.find(next_dirname) != umount_table.end())
                continue;
            x_rm_rf_under(dirfd, next_component, next_dirname);
        }
        const char* op = de->d_type == DT_DIR ? "rmdir" : "rm";
        if (verbose)
            fprintf(verbosefile, "%s %s%s\n", op, dirname.c_str(), de->d_name);
        if (!dryrun && unlinkat(dirfd, de->d_name, de->d_type == DT_DIR ? AT_REMOVEDIR : 0) != 0) {
            fprintf(stderr, "%s %s%s: %s\n", op, dirname.c_str(), de->d_name, strerror(errno));
            exit(1);
        }
    }
    closedir(dir);
    close(dirfd);
}

static std::string take_word(const std::string& str, size_t& pos) {
    while (pos < str.length() && str[pos] != '\n' && isspace((unsigned char) str[pos]))
        ++pos;
    if (pos < str.length() && !isspace((unsigned char) str[pos])) {
        size_t spos = pos;
        while (pos < str.length() && !isspace((unsigned char) str[pos]))
            ++pos;
        return str.substr(spos, pos - spos);
    } else
        return std::string();
}

static bool check_shell(const char* shell) {
    bool found = false;
    char* sh;
    while (!found && (sh = getusershell()))
        found = strcmp(sh, shell) == 0;
    endusershell();
    return found;
}

struct jaildirinfo {
    std::string dir;
    std::string parent;
    int parentfd;
    std::string component;
    bool allowed;
    std::string permdir;
    dev_t dev;

    jaildirinfo(const char* str, jailaction action, bool doforce);
    void check();
    void chown_recursive();

private:
    std::string alternate_permfile;

    void check_permfile(int dirfd, struct stat& dirstat, std::string dir);
    void parse_permfile(int conff, std::string thisdir,
                        const char* permfilename, bool islocal);
    void chown_recursive(int dirfd, std::string& dirbuf, int depth, uid_t owner, gid_t group);
};

jaildirinfo::jaildirinfo(const char* str, jailaction action, bool doforce)
    : dir(check_filename(absolute(str))), parentfd(-1), allowed(false),
      dev(-1) {
    if (dir.empty() || dir == "/" || dir[0] != '/') {
        fprintf(stderr, "%s: Bad characters in filename\n", str);
        exit(1);
    }
    dir = path_endslash(dir);

    int fd = open("/etc/pa-jail.conf", O_RDONLY | O_NOFOLLOW);
    if (fd != -1) {
        parse_permfile(fd, "/etc/", "pa-jail.conf", false);
        close(fd);
    }

    size_t last_pos = 0;
    fd = -1;
    bool dryrunning = false;
    while (last_pos != dir.length()) {
        // extract component
        size_t next_pos = last_pos;
        while (next_pos && next_pos < dir.length() && dir[next_pos] != '/')
            ++next_pos;
        if (!next_pos)
            ++next_pos;
        parent = dir.substr(0, last_pos);
        component = dir.substr(last_pos, next_pos - last_pos);
        std::string thisdir = dir.substr(0, next_pos);
        last_pos = next_pos;
        while (last_pos != dir.length() && dir[last_pos] == '/')
            ++last_pos;

        // check whether we are below the permission directory
        bool allowed_here = !permdir.empty()
            && last_pos >= permdir.length()
            && dir.substr(0, permdir.length()) == permdir;

        // open it and swap it in
        if (parentfd >= 0)
            close(parentfd);
        parentfd = fd;
        fd = openat(parentfd, component.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
        if ((fd == -1 && dryrunning)
            || (fd == -1 && allowed_here && errno == ENOENT
                && (action == do_init || action == do_run))) {
            if (v_mkdirat(parentfd, component.c_str(), 0755, thisdir) != 0) {
                fprintf(stderr, "mkdir %s: %s\n", thisdir.c_str(), strerror(errno));
                exit(1);
            }
            fd = openat(parentfd, component.c_str(), O_CLOEXEC | O_NOFOLLOW);
            // turn off suid+sgid on created root directory
            if (last_pos == dir.length() && (fd >= 0 || dryrun)
                && v_fchmod(fd, 0755, thisdir) != 0) {
                fprintf(stderr, "chmod %s: %s\n", thisdir.c_str(), strerror(errno));
                exit(1);
            }
            if (dryrun) {
                dryrunning = true;
                continue;
            }
        }
        if (fd == -1 && errno == ENOENT && action == do_rm && doforce)
            exit(0);
        else if (fd == -1) {
            fprintf(stderr, "%s: %s\n", thisdir.c_str(), strerror(errno));
            exit(1);
        }

        // stat it
        struct stat s;
        if (fstat(fd, &s) != 0) {
            fprintf(stderr, "%s: %s\n", thisdir.c_str(), strerror(errno));
            exit(1);
        }
        if (!S_ISDIR(s.st_mode)) {
            fprintf(stderr, "%s: Not a directory\n", thisdir.c_str());
            exit(1);
        } else if (!allowed_here && last_pos != dir.length()) {
            if (s.st_uid != ROOT) {
                fprintf(stderr, "%s: Not owned by root\n", thisdir.c_str());
                exit(1);
            } else if ((s.st_gid != ROOT && (s.st_mode & S_IWGRP))
                       || (s.st_mode & S_IWOTH)) {
                fprintf(stderr, "%s: Writable by non-root\n", thisdir.c_str());
                exit(1);
            }
        }
        dev = s.st_dev;

        // check for "pa-jail.conf" allowance
        if (parent.length())
            check_permfile(fd, s, thisdir);
    }
    if (!allowed) {
        fprintf(stderr, "%s: No `pa-jail.conf` enables jails here.\n", dir.c_str());
        if (!alternate_permfile.empty())
            fprintf(stderr, "  (Perhaps you need to edit `%s`.)\n", alternate_permfile.c_str());
        exit(1);
    }
    if (fd >= 0)
        close(fd);
}

static bool writable_only_by_root(const struct stat& st) {
    return st.st_uid == ROOT
        && (st.st_gid == ROOT || !(st.st_mode & S_IWGRP))
        && !(st.st_mode & S_IWOTH);
}

void jaildirinfo::check_permfile(int dirfd, struct stat& dirstat,
                                 std::string thisdir) {
    const char* permfilename = "pa-jail.conf";
    int conff = openat(dirfd, permfilename, O_RDONLY | O_NOFOLLOW);
    if (conff == -1 && errno == ENOENT) {
        permfilename = "JAIL61";
        conff = openat(dirfd, permfilename, O_RDONLY | O_NOFOLLOW);
    }
    if (conff == -1 && errno != ENOENT && errno != ELOOP) {
        fprintf(stderr, "%s/%s: %s\n", thisdir.c_str(), permfilename, strerror(errno));
        exit(1);
    }
    if (conff == -1)
        return;
    if (writable_only_by_root(dirstat))
        parse_permfile(conff, thisdir, permfilename, true);
    close(conff);
}

void jaildirinfo::parse_permfile(int conff, std::string thisdir,
                                 const char* permfilename, bool islocal) {
    thisdir = path_endslash(thisdir);

    struct stat st;
    if (fstat(conff, &st) != 0) {
        fprintf(stderr, "%s%s: %s\n", thisdir.c_str(), permfilename, strerror(errno));
        exit(1);
    } else if (!writable_only_by_root(st)) {
        if (!allowed || verbose)
            fprintf(stderr, allowed ? "%s%s: Writable by non-root, ignoring\n" : "%s%s: Writable by non-root\n", thisdir.c_str(), permfilename);
        if (!allowed)
            exit(1);
        return;
    }

    char buf[8192];
    ssize_t nr = read(conff, buf, sizeof(buf));
    std::string str(buf, nr < 0 ? 0 : nr);
    size_t pos = 0;
    int allowed_globally = -1, allowed_locally = -1;
    std::string allowed_permdir;

    while (pos < str.length()) {
        std::string word1 = take_word(str, pos);
        std::string word2 = take_word(str, pos);
        while (take_word(str, pos).length())
            /* do nothing */;
        while (pos < str.length() && str[pos] == '\n')
            ++pos;

        std::string wdir = word2;
        while (wdir.length() > 2 && wdir[0] == '.' && wdir[1] == '/')
            wdir = wdir.substr(2, wdir.length());
        if (wdir.empty() || wdir == ".")
            wdir = thisdir;
        wdir = path_endslash(wdir);
        if (wdir[0] != '/')
            wdir = thisdir + wdir;

        // `superdir` is the prefix of `dir` that has the same number
        // of slashes as `wdir`
        size_t slcount = 0, slpos = 0;
        while ((slpos = wdir.find('/', slpos)) != std::string::npos)
            ++slcount, ++slpos;
        slpos = 0;
        while (slcount > 0 && (slpos = dir.find('/', slpos)) != std::string::npos)
            --slcount, ++slpos;
        std::string superdir = dir.substr(0, slpos);
        bool dirmatch = fnmatch(wdir.c_str(), superdir.c_str(), FNM_PATHNAME | FNM_PERIOD) == 0;

        if (word1 == "disablejail" || word1 == "nojail") {
            if (word2.empty())
                allowed_globally = allowed_locally = 0;
            else if (dirmatch) {
                allowed_locally = 0;
                allowed_permdir = word2;
            }
        } else if (word1 == "enablejail" || word1 == "allowjail") {
            if (word2.empty())
                allowed_globally = 1;
            else if (dirmatch) {
                allowed_locally = 1;
                allowed_permdir = superdir;
            } else
                alternate_permfile = thisdir + permfilename;
        }
    }

    if (allowed_locally > 0) {
        allowed = true;
        permdir = allowed_permdir;
    } else if (allowed_locally == 0) {
        fprintf(stderr, "%s%s: Jails are disabled under %s\n", thisdir.c_str(), permfilename, allowed_permdir.c_str());
        exit(1);
    } else if (allowed_globally > 0 && islocal) {
        allowed = true;
        permdir = thisdir;
    } else if (allowed_globally == 0 && islocal) {
        fprintf(stderr, "%s%s: Jails are disabled here\n", thisdir.c_str(), permfilename);
        exit(1);
    } else if (allowed_globally == 0) {
        fprintf(stderr, "%s%s: Jails are disabled\n", thisdir.c_str(), permfilename);
        exit(1);
    }
}

void jaildirinfo::check() {
    assert(!permdir.empty() && permdir[permdir.length() - 1] == '/');
    assert(dir.substr(0, permdir.length()) == permdir);
}

void jaildirinfo::chown_recursive() {
    populate_mount_table();
    std::string buf = dir;
    int dirfd = openat(parentfd, component.c_str(), O_CLOEXEC | O_NOFOLLOW);
    if (dirfd == -1)
        perror_exit(buf.c_str());
    chown_recursive(dirfd, buf, 0, ROOT, ROOT);
}

void jaildirinfo::chown_recursive(int dirfd, std::string& dirbuf, int depth, uid_t owner, gid_t group) {
    dirbuf = path_endslash(dirbuf);
    size_t dirbuflen = dirbuf.length();

    typedef std::pair<uid_t, gid_t> ug_t;
    std::map<std::string, ug_t>* home_map = NULL;
    if (depth == 1 && dirbuf.length() >= 6
        && memcmp(dirbuf.data() + dirbuf.length() - 6, "/home/", 6) == 0) {
        home_map = new std::map<std::string, ug_t>;
        setpwent();
        while (struct passwd* pw = getpwent()) {
            std::string name;
            if (pw->pw_dir && strncmp(pw->pw_dir, "/home/", 6) == 0
                && strchr(pw->pw_dir + 6, '/') == NULL)
                name = pw->pw_dir + 6;
            else
                name = pw->pw_name;
            (*home_map)[name] = ug_t(pw->pw_uid, pw->pw_gid);
        }
    }

    DIR* dir = fdopendir(dirfd);
    if (!dir) {
        if (errno == ENOENT && depth == 0 && dryrun)
            return;
        perror_exit(dirbuf.c_str());
    }

    struct dirent* de;
    uid_t u;
    gid_t g;

    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        // don't follow symbolic links
        if (de->d_type == DT_LNK) {
            if (x_lchownat(dirfd, de->d_name, owner, group, dirbuf))
                exit(exit_value);
            continue;
        }

        // look up uid/gid
        u = owner, g = group;
        if (home_map) {
            auto it = home_map->find(de->d_name);
            if (it != home_map->end())
                u = it->second.first, g = it->second.second;
        }

        // recurse
        if (de->d_type == DT_DIR) {
            dirbuf += de->d_name;
            auto it = mount_table.find(dirbuf);
            if (it == mount_table.end()) { // not a mount point
                int subdirfd = openat(dirfd, de->d_name, O_CLOEXEC | O_NOFOLLOW);
                if (subdirfd == -1)
                    perror_exit(dirbuf.c_str());
                if (x_fchown(subdirfd, u, g, dirbuf))
                    exit(exit_value);
                chown_recursive(subdirfd, dirbuf, depth + 1, u, g);
            }
            dirbuf.resize(dirbuflen);
        } else if (x_lchownat(dirfd, de->d_name, u, g, dirbuf))
            exit(exit_value);
    }

    closedir(dir);
    delete home_map;
}


class jailownerinfo {
  public:
    uid_t owner;
    gid_t group;
    std::string owner_home;
    std::string owner_sh;

    jailownerinfo();
    ~jailownerinfo();
    void init(const char* owner_name);
    void exec(int argc, char** argv, jaildirinfo& jaildir,
              int inputfd, double timeout);
    int exec_go();

  private:
    const char* newenv[4];
    char** argv;
    jaildirinfo* jaildir;
    int inputfd;
    struct timeval timeout;
    fd_set readset;
    fd_set writeset;
    struct buffer {
        char buf[8192];
        size_t head;
        size_t tail;
        bool input_closed;
        bool input_isfifo;
        bool output_closed;
        int rerrno;
        buffer()
            : head(0), tail(0), input_closed(false), input_isfifo(false),
              output_closed(false), rerrno(0) {
        }
        void transfer_in(int from);
        void transfer_out(int to);
    };
    buffer to_slave;
    buffer from_slave;

    void start_sigpipe();
    void block(int ptymaster);
    int check_child_timeout(pid_t child, bool waitpid);
    void handle_child(pid_t child, int ptymaster);
    void exec_done(pid_t child, int exit_status) __attribute__((noreturn));
};

jailownerinfo::jailownerinfo()
    : owner(ROOT), group(ROOT), argv() {
}

jailownerinfo::~jailownerinfo() {
    delete[] argv;
}

void jailownerinfo::init(const char* owner_name) {
    if (strlen(owner_name) >= 1024) {
        fprintf(stderr, "%s: Username too long\n", owner_name);
        exit(1);
    }

    struct passwd* pwnam = getpwnam(owner_name);
    if (!pwnam) {
        fprintf(stderr, "%s: No such user\n", owner_name);
        exit(1);
    }

    owner = pwnam->pw_uid;
    group = pwnam->pw_gid;
    if (strcmp(pwnam->pw_dir, "/") == 0)
        owner_home = "/home/nobody";
    else if (strncmp(pwnam->pw_dir, "/home/", 6) == 0)
        owner_home = pwnam->pw_dir;
    else {
        fprintf(stderr, "%s: Home directory %s not under /home\n", owner_name, pwnam->pw_dir);
        exit(1);
    }

    if (strcmp(pwnam->pw_shell, "/bin/bash") == 0
        || strcmp(pwnam->pw_shell, "/bin/sh") == 0
        || check_shell(pwnam->pw_shell))
        owner_sh = pwnam->pw_shell;
    else {
        fprintf(stderr, "%s: Shell %s not allowed by /etc/shells\n", owner_name, pwnam->pw_shell);
        exit(1);
    }

    if (owner == ROOT) {
        fprintf(stderr, "%s: Jail user cannot be root\n", owner_name);
        exit(1);
    }
}

#if __linux__
extern "C" {
static int exec_clone_function(void* arg) {
    jailownerinfo* jailowner = static_cast<jailownerinfo*>(arg);
    return jailowner->exec_go();
}
}
#endif

static void write_pid(int p) {
    if (pidfd >= 0) {
        lseek(pidfd, 0, SEEK_SET);
        char buf[1024];
        int l = sprintf(buf, "%d\n", p);
        ssize_t w = write(pidfd, buf, l);
        if (w != l || ftruncate(pidfd, l) != 0)
            perror_exit(pidfilename.c_str());
    }
}

void jailownerinfo::exec(int argc, char** argv, jaildirinfo& jaildir,
                         int inputfd, double timeout) {
    // adjust environment; make sure we have a PATH
    char homebuf[8192];
    sprintf(homebuf, "HOME=%s", owner_home.c_str());
    const char* path = "PATH=/usr/local/bin:/bin:/usr/bin";
    const char* ld_library_path = NULL;
    {
        extern char** environ;
        for (char** eptr = environ; *eptr; ++eptr)
            if (strncmp(*eptr, "PATH=", 5) == 0)
                path = *eptr;
            else if (strncmp(*eptr, "LD_LIBRARY_PATH=", 16) == 0)
                ld_library_path = *eptr;
    }
    int newenvpos = 0;
    newenv[newenvpos++] = path;
    if (ld_library_path)
        newenv[newenvpos++] = ld_library_path;
    newenv[newenvpos++] = homebuf;
    newenv[newenvpos++] = NULL;

    // create command
    delete[] this->argv;
    this->argv = new char*[5 + argc - (optind + 2)];
    if (!this->argv) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    int newargvpos = 0;
    std::string command;
    this->argv[newargvpos++] = (char*) owner_sh.c_str();
    this->argv[newargvpos++] = (char*) "-l";
    this->argv[newargvpos++] = (char*) "-c";
    if (optind + 3 == argc)
        command = argv[optind + 2];
    else {
        command = shell_quote(argv[optind + 2]);
        for (int i = optind + 3; i < argc; ++i)
            command += std::string(" ") + shell_quote(argv[i]);
    }
    this->argv[newargvpos++] = const_cast<char*>(command.c_str());
    this->argv[newargvpos++] = NULL;

    // store other arguments
    this->jaildir = &jaildir;
    this->inputfd = inputfd;
    if (timeout > 0) {
        struct timeval now, delta;
        gettimeofday(&now, 0);
        delta.tv_sec = (long) timeout;
        delta.tv_usec = (long) ((timeout - delta.tv_sec) * 1000000);
        timeradd(&now, &delta, &this->timeout);
    } else
        timerclear(&this->timeout);

    // enter the jail
#if __linux__
    char* new_stack = (char*) malloc(256 * 1024);
    if (!new_stack) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    int child = clone(exec_clone_function, new_stack + 256 * 1024,
                      CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWPID, this);
    if (child == -1)
        perror_exit("clone");
    int child_waitflags = __WALL;
#else
    int child = fork();
    if (child == 0)
        exit(exec_go());
    int child_waitflags = 0;
#endif
    if (child == -1)
        perror_exit("fork");
    write_pid(child);

    int exit_status = 0;
    if (foreground)
        exit_status = x_waitpid(child, child_waitflags);
    else
        pidfd = -1;
    exit(exit_status);
}

int jailownerinfo::exec_go() {
    // chroot, remount /proc
    if (verbose)
        fprintf(verbosefile, "cd %s\n", jaildir->dir.c_str());
    if (!dryrun && chdir(jaildir->dir.c_str()) != 0)
        perror_exit(jaildir->dir.c_str());
    if (verbose)
        fprintf(verbosefile, "chroot .\n");
    if (!dryrun && chroot(".") != 0)
        perror_exit("chroot");
#if __linux__
    {
        auto it = mount_table.find("/proc");
        if (it != mount_table.end() && it->second.allowed)
            handle_mount(it->second, "/proc");
    }
#endif

    // reduce privileges permanently
    if (verbose)
        fprintf(verbosefile, "su %s\n", uid_to_name(owner));
    if (!dryrun && setgid(group) != 0)
        perror_exit("setgid");
    if (!dryrun && setuid(owner) != 0)
        perror_exit("setuid");

    // create a pty
    int ptymaster = -1;
    char* ptyslavename = NULL;
    if (verbose)
        fprintf(verbosefile, "make-pty %s\n", ptyslavename);
    if (!dryrun) {
        if ((ptymaster = posix_openpt(O_RDWR)) == -1)
            perror_exit("posix_openpt");
        if (grantpt(ptymaster) == -1)
            perror_exit("grantpt");
        if (unlockpt(ptymaster) == -1)
            perror_exit("unlockpt");
        if ((ptyslavename = ptsname(ptymaster)) == NULL)
            perror_exit("ptsname");
    }

    // change into their home directory
    if (verbose)
        fprintf(verbosefile, "cd %s\n", owner_home.c_str());
    if (!dryrun && chdir(owner_home.c_str()) != 0)
        perror_exit(owner_home.c_str());

    // check that shell exists
    if (!dryrun) {
        int f = open(owner_sh.c_str(), O_RDONLY);
        if (f < 0)
            perror_exit(("open" + owner_sh).c_str());
        close(f);
    }

    if (verbose) {
        for (int i = 0; newenv[i]; ++i)
            fprintf(verbosefile, "%s ", newenv[i]);
        for (int i = 0; this->argv[i]; ++i)
            fprintf(verbosefile, i ? " %s" : "%s", shell_quote(this->argv[i]).c_str());
        fprintf(verbosefile, "\n");
    }

    if (!dryrun) {
        start_sigpipe();
        pid_t child = fork();
        if (child < 0)
            perror_exit("fork");
        else if (child == 0) {
            close(sigpipe[0]);
            close(sigpipe[1]);

            if (setsid() == -1)
                perror_exit("setsid");

            int ptyslave = open(ptyslavename, O_RDWR);
            if (ptyslave == -1)
                perror_exit(ptyslavename);
#ifdef TIOCGWINSZ
            struct winsize ws;
            ioctl(ptyslave, TIOCGWINSZ, &ws);
            ws.ws_row = 24;
            ws.ws_col = 80;
            ioctl(ptyslave, TIOCSWINSZ, &ws);
#endif
            struct termios tty;
            if (tcgetattr(ptyslave, &tty) >= 0) {
                tty.c_oflag = 0; // no NL->NLCR xlation, no other proc.
                tcsetattr(ptyslave, TCSANOW, &tty);
            }
            dup2(ptyslave, STDIN_FILENO);
            dup2(ptyslave, STDOUT_FILENO);
            dup2(ptyslave, STDERR_FILENO);
            close(ptymaster);
            close(ptyslave);

            // restore all signals to their default actions
            // (e.g., PHP may have ignored SIGPIPE; don't want that
            // to propagate to student code!)
            for (int sig = 1; sig < NSIG; ++sig)
                signal(sig, SIG_DFL);

            if (execve(this->argv[0], (char* const*) this->argv,
                       (char* const*) newenv) != 0) {
                fprintf(stderr, "exec %s: %s\n", owner_sh.c_str(), strerror(errno));
                exit(126);
            }
        } else
            handle_child(child, ptymaster);
    }

    return 0;
}

extern "C" {
void sighandler(int signo) {
    if (signo == SIGTERM)
        got_sigterm = 1;
    char c = (char) signo;
    ssize_t w = write(sigpipe[1], &c, 1);
    (void) w;
}

void cleanup_pidfd(void) {
    if (pidfd >= 0)
        write_pid(0);
}
}

static void make_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void jailownerinfo::start_sigpipe() {
    int r = pipe(sigpipe);
    if (r != 0)
        perror_exit("pipe");
    make_nonblocking(inputfd);
    make_nonblocking(STDOUT_FILENO);
    make_nonblocking(sigpipe[0]);
    make_nonblocking(sigpipe[1]);

    struct sigaction sa;
    sa.sa_handler = sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    FD_ZERO(&readset);
    FD_ZERO(&writeset);
}

void jailownerinfo::buffer::transfer_in(int from) {
    if (tail == sizeof(buf) && head != 0) {
        memmove(buf, &buf[head], tail - head);
        tail -= head;
        head = 0;
    }

    if (from >= 0 && !input_closed && tail != sizeof(buf)) {
        ssize_t nr = read(from, &buf[tail], sizeof(buf) - tail);
        if (nr != 0 && nr != -1)
            tail += nr;
        else if (nr == 0 && !input_isfifo) {
            // don't want to give up on input if it's a fifo
            struct stat st;
            if (fstat(from, &st) == 0 && S_ISFIFO(st.st_mode))
                input_isfifo = true;
            else
                input_closed = true;
        } else if (nr == -1 && errno != EINTR && errno != EAGAIN) {
            input_closed = true;
            rerrno = errno;
        }
    }
}

void jailownerinfo::buffer::transfer_out(int to) {
    if (to >= 0 && !output_closed && head != tail) {
        ssize_t nw = write(to, &buf[head], tail - head);
        if (nw != 0 && nw != -1)
            head += nw;
        else if (errno != EINTR && errno != EAGAIN)
            output_closed = true;
    }
}

void jailownerinfo::block(int ptymaster) {
    int maxfd = sigpipe[0];
    FD_SET(sigpipe[0], &readset);

    if (!to_slave.input_closed && !to_slave.output_closed) {
        FD_SET(inputfd, &readset);
        maxfd < inputfd && (maxfd = inputfd);
    } else
        FD_CLR(inputfd, &readset);
    if (!to_slave.output_closed && to_slave.head != to_slave.tail) {
        FD_SET(ptymaster, &writeset);
        maxfd < ptymaster && (maxfd = ptymaster);
    } else
        FD_CLR(ptymaster, &writeset);

    if (!from_slave.input_closed && !from_slave.output_closed) {
        FD_SET(ptymaster, &readset);
        maxfd < ptymaster && (maxfd = ptymaster);
    } else
        FD_CLR(ptymaster, &readset);
    if (!from_slave.output_closed && from_slave.head != from_slave.tail) {
        FD_SET(STDOUT_FILENO, &writeset);
        maxfd < STDOUT_FILENO && (maxfd = STDOUT_FILENO);
    } else
        FD_CLR(STDOUT_FILENO, &writeset);

    if (timerisset(&timeout)) {
        struct timeval delay;
        gettimeofday(&delay, 0);
        timersub(&timeout, &delay, &delay);
        select(maxfd + 1, &readset, &writeset, NULL, &delay);
    } else
        select(maxfd + 1, &readset, &writeset, NULL, NULL);

    if (FD_ISSET(sigpipe[0], &readset)) {
        char buf[128];
        while (read(sigpipe[0], buf, sizeof(buf)) > 0)
            /* skip */;
    }
}

int jailownerinfo::check_child_timeout(pid_t child, bool waitpid) {
    if (waitpid) {
        int r = x_waitpid(child, WNOHANG);
        if (r != -1)
            return r;
        else if (errno != EAGAIN)
            return 125;
    }

    if (got_sigterm)
        return 128 + SIGTERM;

    struct timeval now;
    if (timerisset(&timeout)
        && gettimeofday(&now, NULL) == 0
        && timercmp(&now, &timeout, >))
        return 124;

    errno = EAGAIN;
    return -1;
}

void jailownerinfo::handle_child(pid_t child, int ptymaster) {
    // blocking reads please (well, block for up to 0.5sec)
    // the 0.5sec wait means we avoid long race conditions
    struct termios tty;
    if (tcgetattr(ptymaster, &tty) >= 0) {
        tty.c_cc[VMIN] = 1;
        tty.c_cc[VTIME] = 5;
        tcsetattr(ptymaster, TCSANOW, &tty);
    }
    make_nonblocking(ptymaster);
    fflush(stdout);

    while (1) {
        block(ptymaster);
        to_slave.transfer_in(inputfd);
        if (to_slave.head != to_slave.tail
            && memmem(&to_slave.buf[to_slave.head], to_slave.tail - to_slave.head,
                      "\x1b\x03", 2) != NULL)
            exec_done(child, 128 + SIGTERM);
        to_slave.transfer_out(ptymaster);
        from_slave.transfer_in(ptymaster);
        from_slave.transfer_out(STDOUT_FILENO);

        // check child and timeout
        // (only wait for child if read done/failed)
        int exit_status = check_child_timeout(child, from_slave.input_closed);
        if (exit_status != -1)
            exec_done(child, exit_status);

        // if child has not died, and read produced error, report it
        if (from_slave.input_closed && from_slave.rerrno != EIO) {
            fprintf(stderr, "read: %s\n", strerror(from_slave.rerrno));
            exec_done(child, 125);
        }
    }
}

void jailownerinfo::exec_done(pid_t child, int exit_status) {
    if (exit_status == 124 && !quiet)
        fprintf(stdout, "\n\x1b[3;7;31m...timed out\x1b[0m\n");
    if (exit_status == 128 + SIGTERM && !quiet)
        fprintf(stdout, "\n\x1b[3;7;31m...terminated\x1b[0m\n");
#if !__linux__
    if (exit_status >= 124)
        kill(child, SIGKILL);
#else
    (void) child;
#endif
    fflush(stdout);
    exit(exit_status);
}


static __attribute__((noreturn)) void usage() {
    fprintf(stderr, "Usage: pa-jail init [-n] [-f FILES] [-S SKELETON] JAILDIR [USER]\n");
    fprintf(stderr, "       pa-jail run [--fg] [-nq] [-T TIMEOUT] [-p PIDFILE] [-i INPUT] \\\n");
    fprintf(stderr, "                   [-f FILES] [-S SKELETON] JAILDIR USER COMMAND\n");
    fprintf(stderr, "       pa-jail mv OLDDIR NEWDIR\n");
    fprintf(stderr, "       pa-jail rm [-nf] JAILDIR\n");
    exit(1);
}

static struct option longoptions_before[] = {
    { "verbose", no_argument, NULL, 'V' },
    { "dry-run", no_argument, NULL, 'n' },
    { "help", no_argument, NULL, 'H' },
    { NULL, 0, NULL, 0 }
};

static struct option longoptions_run[] = {
    { "verbose", no_argument, NULL, 'V' },
    { "dry-run", no_argument, NULL, 'n' },
    { "help", no_argument, NULL, 'H' },
    { "skeleton", required_argument, NULL, 'S' },
    { "pid-file", required_argument, NULL, 'p' },
    { "files", required_argument, NULL, 'f' },
    { "replace", no_argument, NULL, 'r' },
    { "fg", no_argument, NULL, 'F' },
    { "timeout", required_argument, NULL, 'T' },
    { "input", required_argument, NULL, 'i' },
    { NULL, 0, NULL, 0 }
};

static struct option longoptions_rm[] = {
    { "verbose", no_argument, NULL, 'V' },
    { "dry-run", no_argument, NULL, 'n' },
    { "help", no_argument, NULL, 'H' },
    { "force", no_argument, NULL, 'f' },
    { NULL, 0, NULL, 0 }
};

static struct option* longoptions_action[] = {
    longoptions_before, longoptions_run, longoptions_run, longoptions_rm, longoptions_before
};
static const char* shortoptions_action[] = {
    "+Vn", "VnS:f:p:rT:qi:", "VnS:f:p:rT:qi:", "Vnf", "Vn"
};

int main(int argc, char** argv) {
    // parse arguments
    jailaction action = do_start;
    bool dokill = false, doforce = false;
    double timeout = -1;
    std::string filesarg, inputarg;

    int ch;
    while (1) {
        while ((ch = getopt_long(argc, argv, shortoptions_action[(int) action],
                                 longoptions_action[(int) action], NULL)) != -1) {
            if (ch == 'V')
                verbose = true;
            else if (ch == 'S')
                linkdir = path_noendslash(optarg);
            else if (ch == 'n')
                verbose = dryrun = true;
            else if (ch == 'f' && action == do_rm)
                doforce = true;
            else if (ch == 'f')
                filesarg = optarg;
            else if (ch == 'p')
                pidfilename = optarg;
            else if (ch == 'i')
                inputarg = optarg;
            else if (ch == 'r')
                dokill = true;
            else if (ch == 'F')
                foreground = true;
            else if (ch == 'q')
                quiet = true;
            else if (ch == 'T') {
                char* end;
                timeout = strtod(optarg, &end);
                if (end == optarg || *end != 0)
                    usage();
            } else /* if (ch == 'H') */
                usage();
        }
        if (action != do_start)
            break;
        if (optind == argc)
            usage();
        else if (strcmp(argv[optind], "rm") == 0)
            action = do_rm;
        else if (strcmp(argv[optind], "mv") == 0)
            action = do_mv;
        else if (strcmp(argv[optind], "init") == 0)
            action = do_init;
        else if (strcmp(argv[optind], "run") == 0)
            action = do_run;
        else
            usage();
        argc -= optind;
        argv += optind;
        optind = 1;
    }

    // check arguments
    if (action == do_run && optind + 2 >= argc)
        action = do_init;
    if ((action == do_rm && optind != argc - 1)
        || (action == do_mv && optind != argc - 2)
        || (action == do_init && optind != argc - 1 && optind != argc - 2)
        || (action == do_run && optind > argc - 3)
        || (action == do_rm && (!linkdir.empty() || !filesarg.empty() || !inputarg.empty()))
        || (action == do_mv && (!linkdir.empty() || !filesarg.empty() || !inputarg.empty() || dokill))
        || !argv[optind][0]
        || (action == do_mv && !argv[optind+1][0]))
        usage();
    if (verbose && !dryrun)
        verbosefile = stderr;

    // parse user
    jailownerinfo jailuser;
    if ((action == do_init || action == do_run) && optind + 1 < argc)
        jailuser.init(argv[optind + 1]);

    // open file list as current user
    FILE* filesf = NULL;
    if (filesarg == "-") {
        filesf = stdin;
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, "stdin: Is a tty\n");
            exit(1);
        }
    } else if (!filesarg.empty()) {
        filesf = fopen(filesarg.c_str(), "r");
        if (!filesf) {
            fprintf(stderr, "%s: %s\n", filesarg.c_str(), strerror(errno));
            exit(1);
        }
    }

    // open infile non-blocking as current user
    int inputfd = 0;
    if (!inputarg.empty() && !dryrun) {
        inputfd = open(inputarg.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (inputfd == -1) {
            fprintf(stderr, "%s: %s\n", inputarg.c_str(), strerror(errno));
            exit(1);
        }
    }

    // open pidfile as current user
    if (!pidfilename.empty() && verbose)
        fprintf(verbosefile, "touch %s\n", pidfilename.c_str());
    if (!pidfilename.empty() && !dryrun) {
        pidfd = open(pidfilename.c_str(), O_WRONLY | O_CLOEXEC | O_CREAT | O_TRUNC, 0666);
        if (pidfd == -1) {
            fprintf(stderr, "%s: %s\n", pidfilename.c_str(), strerror(errno));
            exit(1);
        }
        atexit(cleanup_pidfd);
    }

    // escalate so that the real (not just effective) UID/GID is root. this is
    // so that the system processes will execute as root
    uid_t caller_owner = getuid();
    gid_t caller_group = getgid();
    if (!dryrun && setgid(ROOT) < 0)
        perror_exit("setgid");
    if (!dryrun && setuid(ROOT) < 0)
        perror_exit("setuid");

    // check the jail directory
    // - no special characters
    // - path has no symlinks
    // - at least one permdir has a file `pa-jail.conf` owned by root
    //   and writable only by root, that contains `enablejail`
    // - everything above that dir is owned by by root and writable only by
    //   root
    // - no permdir has a file `pa-jail.conf` not owned by root,
    //   writable by other than root, or containing `disablejail`
    // - stuff below the dir containing the allowing `pa-jail.conf`
    //   dynamically created if necessary
    // - try to eliminate TOCTTOU
    jaildirinfo jaildir(argv[optind], action, doforce);

    // move the sandbox if asked
    if (action == do_mv) {
        std::string newpath = check_filename(absolute(argv[optind + 1]));
        if (newpath.empty() || newpath[0] != '/') {
            fprintf(stderr, "%s: Bad characters in move destination\n", argv[optind + 1]);
            exit(1);
        } else if (newpath.length() <= jaildir.permdir.length()
                   || newpath.substr(0, jaildir.permdir.length()) != jaildir.permdir) {
            fprintf(stderr, "%s: Not a subdirectory of %s\n", newpath.c_str(), jaildir.permdir.c_str());
            exit(1);
        }

        // allow second argument to be a directory
        struct stat s;
        if (stat(newpath.c_str(), &s) == 0 && S_ISDIR(s.st_mode))
            newpath = path_endslash(newpath) + jaildir.component;

        if (verbose)
            fprintf(verbosefile, "mv %s%s %s\n", jaildir.parent.c_str(), jaildir.component.c_str(), newpath.c_str());
        if (!dryrun && renameat(jaildir.parentfd, jaildir.component.c_str(), jaildir.parentfd, newpath.c_str()) != 0) {
            fprintf(stderr, "mv %s%s %s: %s\n", jaildir.parent.c_str(), jaildir.component.c_str(), newpath.c_str(), strerror(errno));
            exit(1);
        }
        exit(0);
    }

    // kill the sandbox if asked
    if (action == do_rm || dokill) {
        // unmount EVERYTHING mounted in the jail!
        // INCLUDING MY HOME DIRECTORY
        jaildir.dir = path_endslash(jaildir.dir);
        populate_mount_table();
        for (auto it = mount_table.begin(); it != mount_table.end(); ++it)
            if (it->first.length() >= jaildir.dir.length()
                && memcmp(it->first.data(), jaildir.dir.data(),
                          jaildir.dir.length()) == 0)
                handle_umount(it);
        // remove the jail
        x_rm_rf_under(jaildir.parentfd, jaildir.component, jaildir.dir);
        if (action == do_rm) {
            jaildir.dir = jaildir.dir.substr(0, jaildir.dir.length() - 1);
            if (verbose)
                fprintf(verbosefile, "rmdir %s\n", jaildir.dir.c_str());
            if (!dryrun
                && unlinkat(jaildir.parentfd, jaildir.component.c_str(), AT_REMOVEDIR) != 0
                && !(errno == ENOENT && doforce)) {
                fprintf(stderr, "rmdir %s: %s\n", jaildir.dir.c_str(), strerror(errno));
                exit(1);
            }
            exit(0);
        }
    }

    // check link directory
    if (!linkdir.empty() && v_ensuredir(linkdir.c_str(), 0755) < 0)
        perror_exit(linkdir.c_str());
    if (!linkdir.empty())
        linkdir = absolute(linkdir);
    else
        copy_samedev = false;

    // create the home directory
    if (!jailuser.owner_home.empty()) {
        if (v_ensuredir((jaildir.dir + "/home").c_str(), 0755) < 0)
            perror_exit((jaildir.dir + "/home").c_str());
        std::string jailhome = jaildir.dir + jailuser.owner_home;
        int r = v_ensuredir(jailhome.c_str(), 0700);
        uid_t want_owner = action == do_init ? caller_owner : jailuser.owner;
        gid_t want_group = action == do_init ? caller_group : jailuser.group;
        if (r < 0
            || (r > 0 && x_lchown(jailhome.c_str(), want_owner, want_group)))
            perror_exit(jailhome.c_str());
    }

    // set ownership
    if (action == do_run)
        jaildir.chown_recursive();
    dstroot = path_noendslash(jaildir.dir);
    assert(dstroot != "/");

    // construct the jail
    if (filesf) {
        mode_t old_umask = umask(0);
        if (construct_jail(jaildir.dev, filesf) != 0)
            exit(1);
        fclose(filesf);
        umask(old_umask);
    }

    // close `parentfd`
    close(jaildir.parentfd);
    jaildir.parentfd = -1;

    // maybe execute a command in the jail
    if (optind + 2 < argc)
        jailuser.exec(argc, argv, jaildir, inputfd, timeout);

    exit(0);
}
