/*
    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <sys/utsname.h>
#include "internal_libreport.h"

// Locking logic:
//
// The directory is locked by creating a symlink named .lock inside it,
// whose value (where it "points to") is the pid of locking process.
// We use symlink, not an ordinary file, because symlink creation
// is an atomic operation.
//
// There are two cases where after .lock creation, we might discover
// that directory is not really free:
// * another process just created new directory, but didn't manage
//   to lock it before us.
// * another process is deleting the directory, and we managed to sneak in
//   and create .lock after it deleted all files (including .lock)
//   but before it rmdir'ed the empty directory.
//
// Both these cases are detected by the fact that file named "time"
// is not present (it must be present in any valid dump dir).
// If after locking the dir we don't see time file, we remove the lock
// at once and back off. What happens in concurrent processes
// we interfered with?
// * "create new dump dir" process just re-tries locking.
// * "delete dump dir" process just retries rmdir.
//
// There is another case when we don't find time file:
// when the directory is not really a *dump* dir - user gave us
// an ordinary directory name by mistake.
// We detect it by bailing out of "lock, check time file; sleep
// and retry if it doesn't exist" loop using a counter.
//
// To make locking work reliably, it's important to set timeouts
// correctly. For example, dd_create should retry locking
// its newly-created directory much faster than dd_opendir
// tries to lock the directory it tries to open.


// How long to sleep between "symlink fails with EEXIST,
// readlink fails with ENOENT" tries. Someone just unlocked the dir.
// We never bail out in this case, we retry forever.
// The value can be really small:
#define SYMLINK_RETRY_USLEEP           (10*1000)

// How long to sleep when lock file with valid pid is seen by dd_opendir
// (we are waiting for other process to unlock or die):
#define WAIT_FOR_OTHER_PROCESS_USLEEP (500*1000)

// How long to sleep when lock file with valid pid is seen by dd_create
// (some idiot jumped the gun and locked the dir we just created).
// Must not be the same as WAIT_FOR_OTHER_PROCESS_USLEEP (we depend on this)
// and should be small (we have the priority in locking, this is OUR dir):
#define CREATE_LOCK_USLEEP             (10*1000)

// How long to sleep after we locked a dir, found no time file
// (either we are racing with someone, or it's not a dump dir)
// and unlocked it;
// and after how many tries to give up and declare it's not a dump dir:
#define NO_TIME_FILE_USLEEP            (50*1000)
#define NO_TIME_FILE_COUNT                   10

// How long to sleep after we unlocked an empty dir, but then rmdir failed
// (some idiot jumped the gun and locked the dir we are deleting);
// and after how many tries to give up:
#define RMDIR_FAIL_USLEEP              (10*1000)
#define RMDIR_FAIL_COUNT                     50


static char *load_text_file(const char *path, unsigned flags);
static void copy_file_from_chroot(struct dump_dir* dd, const char *name,
        const char *chroot_dir, const char *file_path);

static bool isdigit_str(const char *str)
{
    do
    {
        if (*str < '0' || *str > '9') return false;
        str++;
    } while (*str);
    return true;
}

static bool exist_file_dir(const char *path)
{
    struct stat buf;
    if (stat(path, &buf) == 0)
    {
        if (S_ISDIR(buf.st_mode) || S_ISREG(buf.st_mode))
        {
            return true;
        }
    }
    return false;
}

/* Returns value less than 0 if the file is not readable or
 * if the file doesn't contain valid unixt time stamp.
 *
 * Any possible failure will be logged.
 */
static long int parse_time_file(const char *filename)
{
    /* Open input file, and parse it. */
    int fd = open(filename, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
    {
        VERB2 perror_msg("Can't open '%s'", filename);
        return -1;
    }

    /* ~ maximal number of digits for positive time stamp string*/
    char time_buf[sizeof(time_t) * 3 + 1];
    ssize_t rdsz = read(fd, time_buf, sizeof(time_buf));

    /* Just reading, so no need to check the returned value. */
    close(fd);

    if (rdsz == -1)
    {
        VERB2 perror_msg("Can't read from '%s'", filename);
        return -1;
    }
    /* approximate maximal number of digits in timestamp is sizeof(time_t)*3 */
    /* buffer has this size + 1 byte for trailing '\0' */
    /* if whole size of buffer was read then file is bigger */
    /* than string representing maximal time stamp */
    if (rdsz == sizeof(time_buf))
    {
        VERB2 error_msg("File '%s' is too long to be valid unix "
                        "time stamp (max size %zdB)", filename, sizeof(time_buf));
        return -1;
    }

    /* Our tools don't put trailing newline into time file,
     * but we allow such format too:
     */
    if (rdsz > 0 && time_buf[rdsz - 1] == '\n')
        rdsz--;
    time_buf[rdsz] = '\0';

    errno = 0;    /* To distinguish success/failure after call */
    char *endptr;
    long val = strtol(time_buf, &endptr, /* base */ 10);

    /* Check for various possible errors */
    if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
            || (errno != 0 && val == 0))
    {
        VERB2 perror_msg("File '%s' doesn't contain valid unix "
                         "time stamp ('%s')", filename, time_buf);
        return -1;
    }

    if (endptr == time_buf)
    {
        VERB2 error_msg("No digits were found in file '%s'", filename);
        return -1;
    }

    if (*endptr != '\0')
    {
        VERB2 error_msg("Further characters after number: '%s' in file '%s'", endptr, filename);
        return -1;
    }

    if (val < 0)
    {
        VERB2 error_msg("Negative time found in file '%s'", filename);
    }

    /* If we got here, strtol() successfully parsed a number */
    return val;
}

/* Return values:
 * -1: error (in this case, errno is 0 if error message is already logged)
 *  0: failed to lock (someone else has it locked)
 *  1: success
 */
static int get_and_set_lock(const char* lock_file, const char* pid)
{
    while (symlink(pid, lock_file) != 0)
    {
        if (errno != EEXIST)
        {
            if (errno != ENOENT && errno != ENOTDIR && errno != EACCES)
            {
                perror_msg("Can't create lock file '%s'", lock_file);
                errno = 0;
            }
            return -1;
        }

        char pid_buf[sizeof(pid_t)*3 + 4];
        ssize_t r = readlink(lock_file, pid_buf, sizeof(pid_buf) - 1);
        if (r < 0)
        {
            if (errno == ENOENT)
            {
                /* Looks like lock_file was deleted */
                usleep(SYMLINK_RETRY_USLEEP); /* avoid CPU eating loop */
                continue;
            }
            perror_msg("Can't read lock file '%s'", lock_file);
            errno = 0;
            return -1;
        }
        pid_buf[r] = '\0';

        if (strcmp(pid_buf, pid) == 0)
        {
            log("Lock file '%s' is already locked by us", lock_file);
            return 0;
        }
        if (isdigit_str(pid_buf))
        {
            char pid_str[sizeof("/proc/") + sizeof(pid_buf)];
            snprintf(pid_str, sizeof(pid_str), "/proc/%s", pid_buf);
            if (access(pid_str, F_OK) == 0)
            {
                log("Lock file '%s' is locked by process %s", lock_file, pid_buf);
                return 0;
            }
            log("Lock file '%s' was locked by process %s, but it crashed?", lock_file, pid_buf);
        }
        /* The file may be deleted by now by other process. Ignore ENOENT */
        if (unlink(lock_file) != 0 && errno != ENOENT)
        {
            perror_msg("Can't remove stale lock file '%s'", lock_file);
            errno = 0;
            return -1;
        }
    }

    VERB1 log("Locked '%s'", lock_file);
    return 1;
}

static int dd_lock(struct dump_dir *dd, unsigned sleep_usec, int flags)
{
    if (dd->locked)
        error_msg_and_die("Locking bug on '%s'", dd->dd_dirname);

    char pid_buf[sizeof(long)*3 + 2];
    snprintf(pid_buf, sizeof(pid_buf), "%lu", (long)getpid());

    unsigned dirname_len = strlen(dd->dd_dirname);
    char lock_buf[dirname_len + sizeof("/.lock")];
    strcpy(lock_buf, dd->dd_dirname);
    strcpy(lock_buf + dirname_len, "/.lock");

    unsigned count = NO_TIME_FILE_COUNT;
 retry:
    while (1)
    {
        int r = get_and_set_lock(lock_buf, pid_buf);
        if (r < 0)
            return r; /* error */
        if (r > 0)
            break; /* locked successfully */
        /* Other process has the lock, wait for it to go away */
        usleep(sleep_usec);
    }

    /* Are we called by dd_opendir (as opposed to dd_create)? */
    if (sleep_usec == WAIT_FOR_OTHER_PROCESS_USLEEP) /* yes */
    {
        strcpy(lock_buf + dirname_len, "/"FILENAME_TIME);
        dd->dd_time = parse_time_file(lock_buf);
        if (dd->dd_time < 0)
        {
            /* time file doesn't exist. We managed to lock the directory
             * which was just created by somebody else, or is almost deleted
             * by delete_file_dir.
             * Unlock and back off.
             */
            strcpy(lock_buf + dirname_len, "/.lock");
            xunlink(lock_buf);
            VERB1 log("Unlocked '%s' (no or corrupted time file)", lock_buf);
            if (--count == 0 || flags & DD_DONT_WAIT_FOR_LOCK)
            {
                errno = EISDIR; /* "this is an ordinary dir, not dump dir" */
                return -1;
            }
            usleep(NO_TIME_FILE_USLEEP);
            goto retry;
        }
    }

    dd->locked = true;
    return 0;
}

static void dd_unlock(struct dump_dir *dd)
{
    if (dd->locked)
    {
        dd->locked = 0;

        unsigned dirname_len = strlen(dd->dd_dirname);
        char lock_buf[dirname_len + sizeof("/.lock")];
        strcpy(lock_buf, dd->dd_dirname);
        strcpy(lock_buf + dirname_len, "/.lock");
        xunlink(lock_buf);

        VERB1 log("Unlocked '%s'", lock_buf);
    }
}

static inline struct dump_dir *dd_init(void)
{
    struct dump_dir* dd = (struct dump_dir*)xzalloc(sizeof(struct dump_dir));
    dd->dd_time = -1;
    return dd;
}

int dd_exist(const struct dump_dir *dd, const char *path)
{
    char *full_path = concat_path_file(dd->dd_dirname, path);
    int ret = exist_file_dir(full_path);
    free(full_path);
    return ret;
}

void dd_close(struct dump_dir *dd)
{
    if (!dd)
        return;

    dd_unlock(dd);
    if (dd->next_dir)
    {
        closedir(dd->next_dir);
        /* free(dd->next_dir); - WRONG! */
    }

    free(dd->dd_dirname);
    free(dd);
}

static char* rm_trailing_slashes(const char *dir)
{
    unsigned len = strlen(dir);
    while (len != 0 && dir[len-1] == '/')
        len--;
    return xstrndup(dir, len);
}

struct dump_dir *dd_opendir(const char *dir, int flags)
{
    struct dump_dir *dd = dd_init();

    dir = dd->dd_dirname = rm_trailing_slashes(dir);

    struct stat stat_buf;
    if (stat(dir, &stat_buf) != 0)
        goto cant_access;
    /* & 0666 should remove the executable bit */
    dd->mode = (stat_buf.st_mode & 0666);

    errno = 0;
    if (dd_lock(dd, WAIT_FOR_OTHER_PROCESS_USLEEP, flags) < 0)
    {
        if ((flags & DD_OPEN_READONLY) && errno == EACCES)
        {
            /* Directory is not writable. If it seems to be readable,
             * return "read only" dd, not NULL */
            if (stat(dir, &stat_buf) == 0
             && S_ISDIR(stat_buf.st_mode)
             && access(dir, R_OK) == 0
            ) {
                char *time_file_name = concat_path_file(dir, FILENAME_TIME);
                dd->dd_time = parse_time_file(time_file_name);
                if (dd->dd_time < 0)
                {
                    dd_close(dd);
                    dd = NULL;
                }
                free(time_file_name);
                return dd;
            }
        }
        if (errno == EISDIR)
        {
            /* EISDIR: dd_lock can lock the dir, but it sees no time file there,
             * even after it retried many times. It must be an ordinary directory!
             *
             * Without this check, e.g. abrt-action-print happily prints any current
             * directory when run without arguments, because its option -d DIR
             * defaults to "."!
             */
            error_msg("'%s' is not a problem directory", dir);
        }
        else if (errno == ENOENT || errno == ENOTDIR)
        {
            if (!(flags & DD_FAIL_QUIETLY_ENOENT))
                error_msg("'%s' does not exist", dir);
        }
        else
        {
            if (!(flags & DD_FAIL_QUIETLY_EACCES))
            {
 cant_access:
                perror_msg("Can't access '%s'", dir);
            }
        }
        dd_close(dd);
        return NULL;
    }

    dd->dd_uid = (uid_t)-1L;
    dd->dd_gid = (gid_t)-1L;
    if (geteuid() == 0)
    {
        /* In case caller would want to create more files, he'll need uid:gid */
        struct stat stat_buf;
        if (stat(dir, &stat_buf) != 0 || !S_ISDIR(stat_buf.st_mode))
        {
            error_msg("Can't stat '%s', or it is not a directory", dir);
            dd_close(dd);
            return NULL;
        }
        dd->dd_uid = stat_buf.st_uid;
        dd->dd_gid = stat_buf.st_gid;
    }

    return dd;
}

/* Create a fresh empty debug dump dir.
 *
 * ABRT owns dump dir:
 *   We should not allow users to write new files or write into existing ones,
 *   but they should be able to read them.
 *
 *   We set dir's gid to passwd(uid)->pw_gid parameter, and we set uid to
 *   abrt's user id. We do not allow write access to group. We can't set dir's
 *   uid to crashed applications's user uid because owner can modify dir's
 *   mode and ownership.
 *
 *   Advantages:
 *   Safeness
 *
 *   Disadvantages:
 *   This approach leads to stealing of directories because events requires
 *   write access to a dump directory and events are run under non root (abrt)
 *   user while reporting.
 *
 *   This approach allows group members to see crashes of other members.
 *   Institutions like schools uses one common group for all students.
 *
 * User owns dump dir:
 *   We grant ownership of dump directories to the user (read/write access).
 *
 *   We set set dir's uid to crashed applications's user uid, and we set gid to
 *   abrt's group id. We allow write access to group because we want to allow
 *   abrt binaries to process dump directories.
 *
 *   Advantages:
 *   No disadvantages from the previous approach
 *
 *   Disadvantages:
 *   In order to protect the system dump directories must be saved on
 *   noncritical filesystem (e.g. /tmp or /var/tmp).
 *
 *
 * @param uid
 *   Crashed application's User Id
 *
 * We currently have only three callers:
 *  kernel oops hook: uid -> not saved, so everyone can steal and work with it
 *   this hook runs under 0:0
 *
 *  ccpp hook: uid=uid of crashed user's binary
 *   this hook runs under 0:0
 *
 *  create_dump_dir_from_problem_data() function:
 *   Currently known callers:
 *    abrt server: uid=uid of user's executable
 *     this runs under 0:0
 *     - clinets: python hook, ruby hook
 *    abrt dbus: uid=uid of user's executable
 *     this runs under 0:0
 *     - clients: setroubleshootd, abrt python
 */
struct dump_dir *dd_create(const char *dir, uid_t uid, mode_t mode)
{
    /* a little trick to copy read bits from file mode to exec bit of dir mode*/
    mode_t dir_mode = mode | ((mode & 0444) >> 2);
    struct dump_dir *dd = dd_init();

    dd->mode = mode;

    /* Unlike dd_opendir, can't use realpath: the directory doesn't exist yet,
     * realpath will always return NULL. We don't really have to:
     * dd_opendir(".") makes sense, dd_create(".") does not.
     */
    dir = dd->dd_dirname = rm_trailing_slashes(dir);

    const char *last_component = strrchr(dir, '/');
    if (last_component)
        last_component++;
    else
        last_component = dir;
    if (dot_or_dotdot(last_component))
    {
        /* dd_create("."), dd_create(".."), dd_create("dir/."),
         * dd_create("dir/..") and similar are madness, refuse them.
         */
        error_msg("Bad dir name '%s'", dir);
        dd_close(dd);
        return NULL;
    }

    /* Was creating it with mode 0700 and user as the owner, but this allows
     * the user to replace any file in the directory, changing security-sensitive data
     * (e.g. "uid", "analyzer", "executable")
     */
    if (g_mkdir_with_parents(dd->dd_dirname, dir_mode) != 0)
    {
        perror_msg("Can't create directory '%s'", dir);
        dd_close(dd);
        return NULL;
    }

    if (dd_lock(dd, CREATE_LOCK_USLEEP, /*flags:*/ 0) < 0)
    {
        dd_close(dd);
        return NULL;
    }

    /* mkdir's mode (above) can be affected by umask, fix it */
    if (chmod(dir, dir_mode) == -1)
    {
        perror_msg("Can't change mode of '%s'", dir);
        dd_close(dd);
        return NULL;
    }

    dd->dd_uid = (uid_t)-1L;
    dd->dd_gid = (gid_t)-1L;
    if (uid != (uid_t)-1L)
    {
        dd->dd_uid = 0;
        dd->dd_uid = 0;

#if DUMP_DIR_OWNED_BY_USER > 0
        /* Check crashed application's uid */
        struct passwd *pw = getpwuid(uid);
        if (pw)
            dd->dd_uid = pw->pw_uid;
        else
            error_msg("User %lu does not exist, using uid 0", (long)uid);

        /* Get ABRT's group gid */
        struct group *gr = getgrnam("abrt");
        if (gr)
            dd->dd_gid = gr->gr_gid;
        else
            error_msg("Group 'abrt' does not exist, using gid 0");
#else
        /* Get ABRT's user uid */
        struct passwd *pw = getpwnam("abrt");
        if (pw)
            dd->dd_uid = pw->pw_uid;
        else
            error_msg("User 'abrt' does not exist, using uid 0");

        /* Get crashed application's gid */
        pw = getpwuid(uid);
        if (pw)
            dd->dd_gid = pw->pw_gid;
        else
            error_msg("User %lu does not exist, using gid 0", (long)uid);
#endif

        if (lchown(dir, dd->dd_uid, dd->dd_gid) == -1)
        {
            perror_msg("Can't change '%s' ownership to %lu:%lu", dir,
                       (long)dd->dd_uid, (long)dd->dd_gid);
        }
    }

    return dd;
}

void dd_create_basic_files(struct dump_dir *dd, uid_t uid, const char *chroot_dir)
{
    char long_str[sizeof(long) * 3 + 2];

    char *time_str = dd_load_text_ext(dd, FILENAME_TIME,
                    DD_FAIL_QUIETLY_ENOENT | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE);
    if (!time_str)
    {
        time_t t = time(NULL);
        sprintf(long_str, "%lu", (long)t);
        dd_save_text(dd, FILENAME_TIME, long_str);
    }
    free(time_str);

    /* it doesn't make sense to create the uid file if uid == -1 */
    if (uid != (uid_t)-1L)
    {
        snprintf(long_str, sizeof(long_str), "%li", (long)uid);
        dd_save_text(dd, FILENAME_UID, long_str);
    }

    struct utsname buf;
    uname(&buf); /* never fails */
    dd_save_text(dd, FILENAME_KERNEL, buf.release);
    dd_save_text(dd, FILENAME_ARCHITECTURE, buf.machine);
    dd_save_text(dd, FILENAME_HOSTNAME, buf.nodename);

    char *release = load_text_file("/etc/os-release",
                        DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE | DD_OPEN_FOLLOW);
    if (release)
    {
        dd_save_text(dd, FILENAME_OS_INFO, release);
        free(release);
    }

    if (chroot_dir)
        copy_file_from_chroot(dd, FILENAME_OS_INFO_IN_ROOTDIR, chroot_dir, "/etc/os-release");

    /* if release exists in dumpdir don't create it, but don't warn
     * if it doesn't
     * i.e: anaconda doesn't have /etc/{fedora,redhat}-release and trying to load it
     * results in errors: rhbz#725857
     */
    release = dd_load_text_ext(dd, FILENAME_OS_RELEASE,
                    DD_FAIL_QUIETLY_ENOENT | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE);

    if (!release)
    {
        release = load_text_file("/etc/system-release",
                DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE | DD_OPEN_FOLLOW);
        if (!release)
            release = load_text_file("/etc/redhat-release",
                    DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE | DD_OPEN_FOLLOW);
        if (!release)
            release = load_text_file("/etc/SuSE-release", DD_OPEN_FOLLOW);

        char *newline = strchr(release, '\n');
        if (newline)
            *newline = '\0';

        dd_save_text(dd, FILENAME_OS_RELEASE, release);
        if (chroot_dir)
            copy_file_from_chroot(dd, FILENAME_OS_RELEASE_IN_ROOTDIR, chroot_dir, "/etc/system-release");
    }
    free(release);
}

void dd_sanitize_mode_and_owner(struct dump_dir *dd)
{
    /* Don't sanitize if we aren't run under root:
     * we assume that during file creation (by whatever means,
     * even by "hostname >file" in abrt_event.conf)
     * normal umask-based mode setting takes care of correct mode,
     * and uid:gid is, of course, set to user's uid and gid.
     *
     * For root operating on /var/spool/abrt/USERS_PROBLEM, this isn't true:
     * "hostname >file", for example, would create file OWNED BY ROOT!
     * This routine resets mode and uid:gid for all such files.
     */
    if (dd->dd_uid == (uid_t)-1)
        return;

    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    DIR *d = opendir(dd->dd_dirname);
    if (!d)
        return;

    struct dirent *dent;
    while ((dent = readdir(d)) != NULL)
    {
        if (dent->d_name[0] == '.') /* ".lock", ".", ".."? skip */
            continue;
        char *full_path = concat_path_file(dd->dd_dirname, dent->d_name);
        struct stat statbuf;
        if (lstat(full_path, &statbuf) == 0 && S_ISREG(statbuf.st_mode))
        {
            if ((statbuf.st_mode & 0777) != dd->mode)
            {
                /* We open the file only for fchmod()
                 *
                 * We use fchmod() because chmod() changes the permissions of
                 * the file specified whose pathname is given in path, which
                 * is dereferenced if it is a symbolic link.
                 */
                int fd = open(full_path, O_RDONLY | O_NOFOLLOW, dd->mode);
                if (fd >= 0)
                {
                    if (fchmod(fd, dd->mode) != 0)
                    {
                        perror_msg("Can't change '%s' mode to 0%o", full_path,
                                   (unsigned)dd->mode);
                    }
                    close(fd);
                }
                else
                {
                    perror_msg("Can't open regular file '%s'", full_path);
                }
            }
            if (statbuf.st_uid != dd->dd_uid || statbuf.st_gid != dd->dd_gid)
            {
                if (lchown(full_path, dd->dd_uid, dd->dd_gid) != 0)
                {
                    perror_msg("Can't change '%s' ownership to %lu:%lu", full_path,
                               (long)dd->dd_uid, (long)dd->dd_gid);
                }
            }
        }
        free(full_path);
    }
    closedir(d);
}

static int delete_file_dir(const char *dir, bool skip_lock_file)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        /* The caller expects us to error out only if the directory
         * still exists (not deleted). If directory
         * *doesn't exist*, return 0 and clear errno.
         */
        if (errno == ENOENT || errno == ENOTDIR)
        {
            errno = 0;
            return 0;
        }
        return -1;
    }

    bool unlink_lock_file = false;
    struct dirent *dent;
    while ((dent = readdir(d)) != NULL)
    {
        if (dot_or_dotdot(dent->d_name))
            continue;
        if (skip_lock_file && strcmp(dent->d_name, ".lock") == 0)
        {
            unlink_lock_file = true;
            continue;
        }
        char *full_path = concat_path_file(dir, dent->d_name);
        if (unlink(full_path) == -1 && errno != ENOENT)
        {
            int err = 0;
            if (errno == EISDIR)
            {
                errno = 0;
                err = delete_file_dir(full_path, /*skip_lock_file:*/ false);
            }
            if (errno || err)
            {
                perror_msg("Can't remove '%s'", full_path);
                free(full_path);
                closedir(d);
                return -1;
            }
        }
        free(full_path);
    }
    closedir(d);

    /* Here we know for sure that all files/subdirs we found via readdir
     * were deleted successfully. If rmdir below fails, we assume someone
     * is racing with us and created a new file.
     */

    if (unlink_lock_file)
    {
        char *full_path = concat_path_file(dir, ".lock");
        xunlink(full_path);
        free(full_path);

        unsigned cnt = RMDIR_FAIL_COUNT;
        do {
            if (rmdir(dir) == 0)
                return 0;
            /* Someone locked the dir after unlink, but before rmdir.
             * This "someone" must be dd_lock().
             * It detects this (by seeing that there is no time file)
             * and backs off at once. So we need to just retry rmdir,
             * with minimal sleep.
             */
            usleep(RMDIR_FAIL_USLEEP);
        } while (--cnt != 0);
    }

    int r = rmdir(dir);
    if (r)
        perror_msg("Can't remove directory '%s'", dir);
    return r;
}

int dd_delete(struct dump_dir *dd)
{
    int r = delete_file_dir(dd->dd_dirname, /*skip_lock_file:*/ true);
    dd->locked = 0; /* delete_file_dir already removed .lock */
    dd_close(dd);
    return r;
}

int dd_chown(struct dump_dir *dd, uid_t new_uid)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    struct stat statbuf;
    if (!(stat(dd->dd_dirname, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)))
    {
        perror_msg("stat('%s')", dd->dd_dirname);
        return 1;
    }

    struct passwd *pw = getpwuid(new_uid);
    if (!pw)
    {
        error_msg("UID %ld is not found in user database", (long)new_uid);
        return 1;
    }

#if DUMP_DIR_OWNED_BY_USER > 0
    uid_t owners_uid = pw->pw_uid;
    gid_t groups_gid = statbuf.st_gid;
#else
    uid_t owners_uid = statbuf.st_uid;
    gid_t groups_gid = pw->pw_gid;
#endif

    int chown_res = lchown(dd->dd_dirname, owners_uid, groups_gid);
    if (chown_res)
        perror_msg("lchown('%s')", dd->dd_dirname);
    else
    {
        dd_init_next_file(dd);
        char *full_name;
        while (chown_res == 0 && dd_get_next_file(dd, /*short_name*/ NULL, &full_name))
        {
            VERB3 log("chowning %s", full_name);
            chown_res = lchown(full_name, owners_uid, groups_gid);
            if (chown_res)
                perror_msg("lchown('%s')", full_name);
            free(full_name);
        }
    }

    return chown_res;
}

static char *load_text_file(const char *path, unsigned flags)
{
    int fd = open(path, O_RDONLY | ((flags & DD_OPEN_FOLLOW) ? 0 : O_NOFOLLOW));
    if (fd == -1)
    {
        if (!(flags & DD_FAIL_QUIETLY_ENOENT))
            perror_msg("Can't open file '%s'", path);
        return (flags & DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE ? NULL : xstrdup(""));
    }

    /* Why? Because half a million read syscalls of one byte each isn't fun.
     * FILE-based IO buffers reads.
     */
    FILE *fp = fdopen(fd, "r");
    if (!fp)
        die_out_of_memory();

    struct strbuf *buf_content = strbuf_new();
    int oneline = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF)
    {
//TODO? \r -> \n?
//TODO? strip trailing spaces/tabs?
        if (ch == '\n')
            oneline = (oneline << 1) | 1;
        if (ch == '\0')
            ch = ' ';
        if (isspace(ch) || ch >= ' ') /* used !iscntrl, but it failed on unicode */
            strbuf_append_char(buf_content, ch);
    }
    fclose(fp); /* this also closes fd */

    char last = oneline != 0 ? buf_content->buf[buf_content->len - 1] : 0;
    if (last == '\n')
    {
        /* If file contains exactly one '\n' and it is at the end, remove it.
         * This enables users to use simple "echo blah >file" in order to create
         * short string items in dump dirs.
         */
        if (oneline == 1)
            buf_content->buf[--buf_content->len] = '\0';
    }
    else /* last != '\n' */
    {
        /* Last line is unterminated, fix it */
        /* Cases: */
        /* oneline=0: "qwe" - DONT fix this! */
        /* oneline=1: "qwe\nrty" - two lines in fact */
        /* oneline>1: "qwe\nrty\uio" */
        if (oneline >= 1)
            strbuf_append_char(buf_content, '\n');
    }

    return strbuf_free_nobuf(buf_content);
}

static void copy_file_from_chroot(struct dump_dir* dd, const char *name, const char *chroot_dir, const char *file_path)
{
    char *chrooted_name = concat_path_file(chroot_dir, file_path);
    char *data = load_text_file(chrooted_name,
                    DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE | DD_OPEN_FOLLOW);
    free(chrooted_name);
    if (data)
    {
        dd_save_text(dd, name, data);
        free(data);
    }
}

static bool save_binary_file(const char *path, const char* data, unsigned size, uid_t uid, gid_t gid, mode_t mode)
{
    /* the mode is set by the caller, see dd_create() for security analysis */
    unlink(path);
    int fd = open(path, O_WRONLY | O_TRUNC | O_CREAT | O_NOFOLLOW, mode);
    if (fd < 0)
    {
        perror_msg("Can't open file '%s'", path);
        return false;
    }

    if (uid != (uid_t)-1L)
    {
        if (fchown(fd, uid, gid) == -1)
        {
            perror_msg("Can't change '%s' ownership to %lu:%lu", path, (long)uid, (long)gid);
        }
    }

    /* O_CREATE in the open() call above causes that the permissions of the
     * created file are (mode & ~umask)
     *
     * This is true only if we did create file. We are not sure we created it
     * in this case - it may exist already.
     */
    if (fchmod(fd, mode) == -1)
    {
        perror_msg("Can't change mode of '%s'", path);
    }

    unsigned r = full_write(fd, data, size);
    close(fd);
    if (r != size)
    {
        error_msg("Can't save file '%s'", path);
        return false;
    }

    return true;
}

char* dd_load_text_ext(const struct dump_dir *dd, const char *name, unsigned flags)
{
//    if (!dd->locked)
//        error_msg_and_die("dump_dir is not opened"); /* bug */

    /* Compat with old abrt dumps. Remove in abrt-2.1 */
    if (strcmp(name, "release") == 0)
        name = FILENAME_OS_RELEASE;

    char *full_path = concat_path_file(dd->dd_dirname, name);
    char *ret = load_text_file(full_path, flags);
    free(full_path);

    return ret;
}

char* dd_load_text(const struct dump_dir *dd, const char *name)
{
    return dd_load_text_ext(dd, name, /*flags:*/ 0);
}

void dd_save_text(struct dump_dir *dd, const char *name, const char *data)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    char *full_path = concat_path_file(dd->dd_dirname, name);
    save_binary_file(full_path, data, strlen(data), dd->dd_uid, dd->dd_gid, dd->mode);
    free(full_path);
}

void dd_save_binary(struct dump_dir* dd, const char* name, const char* data, unsigned size)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    char *full_path = concat_path_file(dd->dd_dirname, name);
    save_binary_file(full_path, data, size, dd->dd_uid, dd->dd_gid, dd->mode);
    free(full_path);
}

long dd_get_item_size(struct dump_dir *dd, const char *name)
{
    long size = -1;
    char *iname = concat_path_file(dd->dd_dirname, name);
    struct stat statbuf;

    if (lstat(iname, &statbuf) == 0 && S_ISREG(statbuf.st_mode))
        size = statbuf.st_size;
    else
    {
        if (errno == ENOENT)
            size = 0;
        else
            perror_msg("Can't get size of file '%s'", iname);
    }

    free(iname);

    return size;
}

int dd_delete_item(struct dump_dir *dd, const char *name)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    char *path = concat_path_file(dd->dd_dirname, name);
    int res = unlink(path);

    if (res < 0)
    {
        if (errno == ENOENT)
            errno = res = 0;
        else
            perror_msg("Can't delete file '%s'", path);
    }

    free(path);
    return res;
}

DIR *dd_init_next_file(struct dump_dir *dd)
{
//    if (!dd->locked)
//        error_msg_and_die("dump_dir is not opened"); /* bug */

    if (dd->next_dir)
        closedir(dd->next_dir);

    dd->next_dir = opendir(dd->dd_dirname);
    if (!dd->next_dir)
    {
        error_msg("Can't open directory '%s'", dd->dd_dirname);
    }

    return dd->next_dir;
}

int dd_get_next_file(struct dump_dir *dd, char **short_name, char **full_name)
{
    if (dd->next_dir == NULL)
        return 0;

    struct dirent *dent;
    while ((dent = readdir(dd->next_dir)) != NULL)
    {
        if (is_regular_file(dent, dd->dd_dirname))
        {
            if (short_name)
                *short_name = xstrdup(dent->d_name);
            if (full_name)
                *full_name = concat_path_file(dd->dd_dirname, dent->d_name);
            return 1;
        }
    }

    closedir(dd->next_dir);
    dd->next_dir = NULL;
    return 0;
}

/* reported_to handling */

void add_reported_to(struct dump_dir *dd, const char *line)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    char *reported_to = dd_load_text_ext(dd, FILENAME_REPORTED_TO, DD_FAIL_QUIETLY_ENOENT | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE);
    if (reported_to)
    {
        unsigned len_line = strlen(line);
        char *p = reported_to;
        while (*p)
        {
            if (strncmp(p, line, len_line) == 0 && (p[len_line] == '\n' || p[len_line] == '\0'))
                goto ret;
            p = strchrnul(p, '\n');
            if (!*p)
                break;
            p++;
        }
        if (p != reported_to && p[-1] != '\n')
            reported_to = append_to_malloced_string(reported_to, "\n");
        reported_to = append_to_malloced_string(reported_to, line);
        reported_to = append_to_malloced_string(reported_to, "\n");
    }
    else
        reported_to = xasprintf("%s\n", line);
    dd_save_text(dd, FILENAME_REPORTED_TO, reported_to);
 ret:
    free(reported_to);
}

void free_report_result(struct report_result *result)
{
    if (!result)
        return;
    free(result->url);
    free(result->msg);
    free(result->bthash);
    free(result);
}

static report_result_t *parse_reported_line(const char *line)
{
    report_result_t *result = xzalloc(sizeof(*result));

    //result->whole_line = xstrdup(line);
    for (;;)
    {
        line = skip_whitespace(line);
        if (!*line)
            break;
        const char *end = skip_non_whitespace(line);
        if (prefixcmp(line, "MSG=") == 0)
        {
            result->msg = xstrdup(line + 4);
            /* MSG=... eats entire line: exiting the loop */
            break;
        }
        if (prefixcmp(line, "URL=") == 0)
        {
            free(result->url);
            result->url = xstrndup(line + 4, end - (line + 4));
        }
        if (prefixcmp(line, "BTHASH=") == 0)
        {
            free(result->bthash);
            result->bthash = xstrndup(line + 7, end - (line + 7));
        }
        //else
        //if (strncmp(line, "TIME=", 5) == 0)
        //{
        //    free(result->time);
        //    result->time = foo(line + 5, end - (line + 5));
        //}
        //...
        line = end;
        continue;
    }

    return result;
}

report_result_t *find_in_reported_to(struct dump_dir *dd, const char *prefix)
{
    char *reported_to = dd_load_text_ext(dd, FILENAME_REPORTED_TO,
                DD_FAIL_QUIETLY_ENOENT | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE);
    if (!reported_to)
        return NULL;

    /* Find *last* (most recent) line with this prefix */
    unsigned prefix_len = strlen(prefix);
    char *found = NULL;
    char *p = reported_to;
    while (*p)
    {
        if (strncmp(p, prefix, prefix_len) == 0)
            found = p + prefix_len;
        p = strchrnul(p, '\n');
        if (*p)
        {
            *p = '\0'; /* EOL marker for parse_reported_line() below */
            p++;
        }
    }

    report_result_t *result = NULL;
    if (found)
        result = parse_reported_line(found);

    free(reported_to);
    return result;
}

int dd_rename(struct dump_dir *dd, const char *new_path)
{
    int res = rename(dd->dd_dirname, new_path);
    if (res == 0)
    {
        free(dd->dd_dirname);
        dd->dd_dirname = rm_trailing_slashes(new_path);
    }
    return res;
}

/* Utility function */

void delete_dump_dir(const char *dirname)
{
    struct dump_dir *dd = dd_opendir(dirname, /*flags:*/ 0);
    if (dd)
    {
        dd_delete(dd);
    }
}

#if DUMP_DIR_OWNED_BY_USER == 0
static bool uid_in_group(uid_t uid, gid_t gid)
{
    char **tmp;
    struct passwd *pwd = getpwuid(uid);

    if (!pwd)
        return FALSE;

    if (pwd->pw_gid == gid)
        return TRUE;

    struct group *grp = getgrgid(gid);
    if (!(grp && grp->gr_mem))
        return FALSE;

    for (tmp = grp->gr_mem; *tmp != NULL; tmp++)
    {
        if (g_strcmp0(*tmp, pwd->pw_name) == 0)
        {
            VERB3 log("user %s belongs to group: %s",  pwd->pw_name, grp->gr_name);
            return TRUE;
        }
    }

    VERB2 log("user %s DOESN'T belong to group: %s",  pwd->pw_name, grp->gr_name);
    return FALSE;
}
#endif

int dump_dir_accessible_by_uid(const char *dirname, uid_t uid)
{
    struct stat statbuf;
    if (stat(dirname, &statbuf) != 0 || !S_ISDIR(statbuf.st_mode))
        errno = ENOTDIR;
    else
    {
        errno = 0;

#if DUMP_DIR_OWNED_BY_USER > 0
        if (uid == 0 || (statbuf.st_mode & S_IROTH) || uid == statbuf.st_uid)
#else
        if (uid == 0 || (statbuf.st_mode & S_IROTH) || uid_in_group(uid, statbuf.st_gid))
#endif
        {
            VERB1 log("directory '%s' is accessible by %ld uid", dirname, (long)uid);
            return 1;
        }
    }

    return 0;
}

int dd_mark_as_notreportable(struct dump_dir *dd, const char *reason)
{
    if (!dd->locked)
    {
        error_msg("dump_dir is not locked for writing");
        return -1;
    }

    dd_save_text(dd, FILENAME_NOT_REPORTABLE, reason);
    return 0;
}
