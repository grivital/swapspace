/*
This file is part of Swapspace.

Swapspace is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Swapspace is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with swapspace; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/
#include "env.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <linux/loop.h>
#include <linux/stat.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>

#include "cryptdev.h"
#include "log.h"
#include "support.h"

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/// How often to retry when another process grabs the free loop device first
#define LOOP_ATTEMPTS 5

/// Slot limit for reading mapper names; matches MAX_SWAPFILES in swaps.c
#define SLOT_LIMIT 32


/// Read a sysfs "name" file and test it against swapspace-<seqno>
static bool dm_name_is(const char node[], int seqno)
{
  char path[PATH_MAX];
  int found;
  return crypt_sysfs_block_path(path, sizeof(path), node, "dm/name") &&
    crypt_read_mapper_name(path, SLOT_LIMIT, &found) && found == seqno;
}


/* opendir() allocates, as do the fopen() of /proc/swaps and the system() calls
 * elsewhere in the daemon.  This runs when a swapfile is set up, or when a
 * mapping not yet known shows up in /proc/swaps.
 */
bool crypt_find_dm(int seqno, int *dmno)
{
  char dir[PATH_MAX];
  if (unlikely(snprintf(dir, sizeof(dir), "%s/block", crypt_sysfs_root) >=
	(int)sizeof(dir)))
    return false;

  DIR *const d = opendir(dir);
  if (!d) return false;

  bool found = false;
  for (const struct dirent *e = readdir(d); e && !found; e = readdir(d))
  {
    if (strncmp(e->d_name, "dm-", 3) != 0) continue;

    char devnode[PATH_MAX];
    if (snprintf(devnode, sizeof(devnode), "/dev/%s", e->d_name) >=
	(int)sizeof(devnode))
      continue;
    if (crypt_dm_node_number(devnode, dmno) && dm_name_is(e->d_name, seqno))
      found = true;
  }
  closedir(d);
  return found;
}


/// Does a device-mapper device for swapfile seqno exist, with or without a node?
/** A dm device can exist without its /dev/mapper node (udev lagging or
 * failing).  cryptsetup open would then fail on the name forever, so the
 * kernel's own list of dm names is consulted as well.
 */
static bool real_mapping_exists(int seqno)
{
  char path[64];
  int dmno;
  return (crypt_mapper_path(path, sizeof(path), seqno) &&
      access(path, F_OK) == 0) ||
    crypt_find_dm(seqno, &dmno);
}


bool crypt_mapping_exists(int seqno)
{
  return swapops->mapping_exists(seqno);
}


#ifdef LOOP_CONFIGURE
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

/// Direct I/O alignment and preferred I/O size of an open file (0 if unknown)
/** Direct I/O is only possible if the loop device's logical block size is at
 * least the backing file's direct I/O offset alignment.  The loop default is
 * 512 bytes, which makes the kernel refuse direct I/O on 4Kn storage (and on
 * any filesystem that wants 4096-byte aligned direct I/O).  So ask the
 * filesystem (statx STATX_DIOALIGN, Linux 6.1), and report the file's
 * preferred I/O size (the filesystem block size) as the fallback.  Both come
 * from the descriptor that is bound to the loop device, not from the path, so
 * they describe the file that is actually used.
 */
static void io_hints(int fd, unsigned long *dio_align, unsigned long *blksize)
{
  *dio_align = *blksize = 0;

#if defined(SYS_statx) && defined(STATX_DIOALIGN)
  struct statx stx;
  if (syscall(SYS_statx, fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &stx) == 0 &&
      (stx.stx_mask & STATX_DIOALIGN))
    *dio_align = stx.stx_dio_offset_align;
#endif

  struct stat st;
  if (fstat(fd, &st) == 0 && st.st_blksize > 0)
    *blksize = (unsigned long)st.st_blksize;
}


/// The LOOP_CONFIGURE request: bind fd with direct I/O, autoclear and a block size
/** The block size comes from the file's alignment hints (see io_hints() and
 * crypt_loop_blocksize()).  If no block size fits (the direct I/O alignment is
 * above the page size) nothing is requested and false is returned: the caller
 * must refuse, since the kernel would take the alignment as the block size.
 * Swapping over a device whose logical block size is
 * below the page size is fine: mm/swapfile.c (checked against current
 * mainline) only rejects zoned devices, and the older kernels that called
 * set_blocksize(PAGE_SIZE) there needed the block size to be at most the page
 * size, which crypt_loop_blocksize() guarantees.  It matters on systems with
 * 16 KiB pages (Asahi): there the block size is 4096.  Swapfile sizes are
 * multiples of the page size, hence of the block size.
 */
static bool loop_request(struct loop_config *cfg, int backing,
    unsigned long dio_align, unsigned long blksize, unsigned long pagesize)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->fd = backing;
  cfg->block_size = crypt_loop_blocksize(dio_align, blksize, pagesize);
  cfg->info.lo_flags = LO_FLAGS_AUTOCLEAR | LO_FLAGS_DIRECT_IO;
  return cfg->block_size != 0;
}


/// Did the kernel really enable direct I/O?  It drops the flag silently.
static bool loop_dio_active(const struct loop_info64 *info)
{
  return (info->lo_flags & LO_FLAGS_DIRECT_IO) != 0;
}


/// Attach file to a free loop device with direct I/O.
/** Needs Linux 5.8 or later (LOOP_CONFIGURE).  Direct I/O is requested in the
 * same call that binds the file, together with the block size it needs, so the
 * device never exists in a state where direct I/O is impossible.  The device is
 * set to autoclear: the kernel detaches it when the last file descriptor or
 * user (such as a dm-crypt mapping) goes away, so closing *loopfd undoes this
 * on failure paths.
 *
 * @param loopno receives the loop device number
 * @param loopfd receives an open descriptor for the loop device; the caller
 * closes it
 */
static bool real_loop_attach(const char file[], int *loopno, int *loopfd)
{
  const int backing = open(file, O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_LARGEFILE);
  if (unlikely(backing == -1))
  {
    log_perr_str(LOG_ERR, "Could not open swapfile for loop device", file,
	errno);
    return false;
  }

  const int ctl = open("/dev/loop-control", O_RDWR|O_CLOEXEC);
  if (unlikely(ctl == -1))
  {
    log_perr(LOG_ERR, "Could not open /dev/loop-control (is the loop module "
	"available?)", errno);
    close(backing);
    return false;
  }

  unsigned long dio_align, blksize;
  io_hints(backing, &dio_align, &blksize);

  bool ok = false;
  for (int attempt = 0; attempt < LOOP_ATTEMPTS && !ok; ++attempt)
  {
    const int n = ioctl(ctl, LOOP_CTL_GET_FREE);
    if (unlikely(n < 0))
    {
      log_perr(LOG_ERR, "Could not get a free loop device", errno);
      break;
    }

    char dev[32];
    if (unlikely(!crypt_loop_path(dev, sizeof(dev), n))) break;
    const int fd = open(dev, O_RDWR|O_CLOEXEC);
    if (unlikely(fd == -1))
    {
      log_perr_str(LOG_ERR, "Could not open loop device", dev, errno);
      break;
    }

    struct loop_config cfg;
    if (unlikely(!loop_request(&cfg, backing, dio_align, blksize,
	    (unsigned long)getpagesize())))
    {
      logm(LOG_ERR, "Direct I/O alignment %lu of '%s' exceeds the page size; "
	  "refusing to use crypt mode here", dio_align, file);
      close(fd);
      break;
    }
    if (ioctl(fd, LOOP_CONFIGURE, &cfg) == -1)
    {
      const int err = errno;
      close(fd);
      // Someone else took the device between GET_FREE and CONFIGURE.
      if (err == EBUSY) continue;
      log_perr_str(LOG_ERR, "Could not attach loop device", dev, err);
      break;
    }

    /* Direct I/O is mandatory: without it, swap writes would be buffered by
     * the backing file's page cache, which needs memory to make progress.  The
     * kernel silently drops the flag when the filesystem cannot do direct I/O
     * with this block size, so read back what is really in effect.
     */
    struct loop_info64 info;
    if (unlikely(ioctl(fd, LOOP_GET_STATUS64, &info) == -1))
    {
      log_perr_str(LOG_ERR, "Could not read back the loop device state", dev,
	  errno);
      close(fd);	// autoclear detaches the device
      break;
    }
    if (unlikely(!loop_dio_active(&info)))
    {
      logm(LOG_ERR,
	  "Direct I/O is not supported for '%s' by its filesystem or device "
	  "(loop block size %u); refusing to use crypt mode here",
	  file, cfg.block_size);
      close(fd);
      break;
    }

    *loopno = n;
    *loopfd = fd;
    ok = true;
  }

  close(ctl);
  close(backing);
  return ok;
}
#else
static bool real_loop_attach(const char file[], int *loopno, int *loopfd)
{
  logm(LOG_ERR, "Crypt mode needs LOOP_CONFIGURE (Linux 5.8 headers); "
      "swapspace was built without it");
  return false;
}
#endif


static void real_loop_close(int loopfd)
{
  close(loopfd);
}

static int real_run(const char line[])
{
  return runcommandline(line);
}

static int real_mkswap(const char dev[])
{
  return runcommand("mkswap", dev);
}

static int real_swapon(const char dev[])
{
  return swapon(dev, 0);
}

static int real_swapoff(const char dev[])
{
  return swapoff(dev);
}

static const struct swap_ops real_ops =
{
  .loop_attach = real_loop_attach,
  .loop_close = real_loop_close,
  .run = real_run,
  .mapping_exists = real_mapping_exists,
  .mkswap = real_mkswap,
  .swapon = real_swapon,
  .swapoff = real_swapoff,
};

const struct swap_ops *swapops = &real_ops;


enum crypt_result crypt_map_file(const char file[], int seqno)
{
  if (unlikely(swapops->mapping_exists(seqno)))
  {
    logm(LOG_ERR, "Device-mapper name swapspace-%d is already in use", seqno);
    return CRYPT_FAILED;
  }

  int loopno, loopfd;
  if (unlikely(!swapops->loop_attach(file, &loopno, &loopfd)))
  {
    logm(LOG_ERR, "Could not attach swapfile %d to a loop device", seqno);
    return CRYPT_FAILED;
  }

  enum crypt_result result = CRYPT_OK;
  const bool ok =
    crypt_open_cmd(localbuf, sizeof(localbuf), crypt_cipher, crypt_keysize,
	seqno, loopno) &&
    swapops->run(localbuf) == 0;

  if (unlikely(!ok))
  {
    logm(LOG_ERR, "Could not create encrypted mapping swapspace-%d", seqno);
    result = CRYPT_FAILED;
    /* The mapping did not exist before we started, so anything there now is
     * ours.  This covers cryptsetup failing half-way.  If it cannot be removed
     * it still holds the file through the loop device.
     */
    if (swapops->mapping_exists(seqno) && !crypt_unmap(seqno))
      result = CRYPT_HELD;
  }

  /* On success the mapping holds the loop device; on failure this was the last
   * reference and the device detaches.
   */
  swapops->loop_close(loopfd);
  return result;
}


bool crypt_unmap(int seqno)
{
  if (unlikely(!crypt_close_cmd(localbuf, sizeof(localbuf), seqno)))
    return false;

  const int status = swapops->run(localbuf);
  if (unlikely(status != 0))
  {
    logm(LOG_ERR, "cryptsetup close swapspace-%d failed (status %d)", seqno,
	status);
    return false;
  }
  return true;
}


enum crypt_stale crypt_clear_stale(int seqno, const char file[])
{
  if (!swapops->mapping_exists(seqno)) return CRYPT_SLOT_FREE;

  /* The name is global.  Closing a mapping of another instance would take its
   * swap away (or fail while it is in use); only close what sits on our file.
   */
  char dev[64];
  if (!crypt_mapper_path(dev, sizeof(dev), seqno) ||
      !crypt_mapping_backed_by(dev, seqno, file))
    return CRYPT_SLOT_FOREIGN;

  logm(LOG_NOTICE, "Removing leftover mapping swapspace-%d", seqno);
  crypt_unmap(seqno);
  return swapops->mapping_exists(seqno) ? CRYPT_SLOT_HELD : CRYPT_SLOT_FREE;
}


enum crypt_result crypt_enable(int seqno, const char file[])
{
  char dev[64];
  if (unlikely(!crypt_mapper_path(dev, sizeof(dev), seqno)))
    return CRYPT_FAILED;

  const enum crypt_result mapped = crypt_map_file(file, seqno);
  if (mapped != CRYPT_OK) return mapped;

  /* Unlike plain swapfiles, a failing mkswap is fatal: an unformatted device
   * must not be offered to swapon.  Nothing here ever falls back to enabling
   * the plain file.
   */
  const int mkstatus = swapops->mkswap(dev);
  if (unlikely(mkstatus != 0))
  {
    logm(LOG_ERR, "mkswap failed for '%s' (status %d)", dev, mkstatus);
  }
  else if (unlikely(swapops->swapon(dev) != 0))
  {
    const int err = errno;
    log_perr_str(LOG_ERR, "Could not enable encrypted swap", dev, err);
  }
  else
  {
    return CRYPT_OK;
  }

  if (unlikely(!crypt_unmap(seqno)))
  {
    logm(LOG_ERR, "Could not remove mapping swapspace-%d; it will be removed "
	"before this slot is used again", seqno);
    return CRYPT_HELD;
  }
  return CRYPT_FAILED;
}


bool crypt_retire(int seqno)
{
  char dev[64];
  if (unlikely(!crypt_mapper_path(dev, sizeof(dev), seqno))) return false;

  if (unlikely(swapops->swapoff(dev) == -1))
  {
    const int err = errno;
    log_perr_str(LOG_ERR, "Could not disable encrypted swap", dev, err);
    return false;
  }
  return crypt_unmap(seqno);
}


bool crypt_read_mapper_name(const char sysfs_path[], int limit, int *seqno)
{
  const int fd = open(sysfs_path, O_RDONLY|O_CLOEXEC);
  if (fd == -1) return false;

  char name[64];
  const ssize_t len = read(fd, name, sizeof(name) - 1);
  close(fd);
  if (len <= 0) return false;

  name[len] = '\0';
  if (name[len-1] == '\n') name[len-1] = '\0';
  return crypt_parse_mapper_name(name, limit, seqno);
}


/// Read the first line of a sysfs file into buf (without the newline)
static bool read_line(const char path[], char buf[], size_t size)
{
  const int fd = open(path, O_RDONLY|O_CLOEXEC);
  if (fd == -1) return false;

  const ssize_t len = read(fd, buf, size - 1);
  close(fd);
  if (len <= 0) return false;

  buf[len] = '\0';
  char *const nl = strchr(buf, '\n');
  if (nl) *nl = '\0';
  return buf[0] != '\0';
}


/// Device and inode of the file behind loop<loopno>, from sysfs
/** A deleted backing file is reported with a suffix that makes stat() fail. */
static bool loop_backing_identity(int loopno, dev_t *dev, ino_t *ino)
{
  char node[32], path[PATH_MAX], backing[PATH_MAX];
  struct stat st;
  if (snprintf(node, sizeof(node), "loop%d", loopno) >= (int)sizeof(node) ||
      !crypt_sysfs_block_path(path, sizeof(path), node, "loop/backing_file") ||
      !read_line(path, backing, sizeof(backing)) ||
      stat(backing, &st) != 0)
    return false;
  *dev = st.st_dev;
  *ino = st.st_ino;
  return true;
}


/// The loop device number if name is "loop<digits>", else -1
static int loop_number(const char name[])
{
  if (strncmp(name, "loop", 4) != 0 || !name[4] || strlen(name) > 12) return -1;
  int n = 0;
  for (const char *p = name + 4; *p; ++p)
  {
    if (*p < '0' || *p > '9') return -1;
    n = n * 10 + (*p - '0');
  }
  return n;
}


bool crypt_mapping_trace(const char devname[], int seqno,
    struct crypt_mapping *map)
{
  int dmno, n;
  if (!crypt_dm_node_number(devname, &dmno))
  {
    // Listed as /dev/mapper/swapspace-<n>: find the dm device by its name.
    if (!crypt_parse_mapper_path(devname, SLOT_LIMIT, &n) || n != seqno ||
	!crypt_find_dm(seqno, &dmno))
      return false;
  }

  char node[32], path[PATH_MAX];
  if (snprintf(node, sizeof(node), "dm-%d", dmno) >= (int)sizeof(node) ||
      !dm_name_is(node, seqno) ||
      !crypt_sysfs_block_path(path, sizeof(path), node, "slaves"))
    return false;

  // The mapping must sit on exactly one device, and that must be a loop device.
  DIR *const d = opendir(path);
  if (!d) return false;
  int loopno = -1, count = 0;
  for (const struct dirent *e = readdir(d); e; e = readdir(d))
  {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    ++count;
    loopno = loop_number(e->d_name);
  }
  closedir(d);
  if (count != 1 || loopno < 0) return false;

  map->dmno = dmno;
  map->loopno = loopno;
  return loop_backing_identity(loopno, &map->dev, &map->ino);
}


bool crypt_mapping_is(const struct crypt_mapping *map, const char file[])
{
  /* The entry itself, not what it points to: a symlink or a second hard link
   * in our directory must not make us claim another instance's backing file.
   */
  struct stat ours;
  return lstat(file, &ours) == 0 && S_ISREG(ours.st_mode) &&
    ours.st_nlink == 1 && map->dev == ours.st_dev && map->ino == ours.st_ino;
}


bool crypt_mapping_backed_by(const char devname[], int seqno, const char file[])
{
  struct crypt_mapping map;
  return crypt_mapping_trace(devname, seqno, &map) &&
    crypt_mapping_is(&map, file);
}


bool crypt_mapping_unchanged(const struct crypt_mapping *map, int seqno)
{
  char node[32], rest[48], path[PATH_MAX];
  dev_t dev;
  ino_t ino;
  return snprintf(node, sizeof(node), "dm-%d", map->dmno) < (int)sizeof(node) &&
    dm_name_is(node, seqno) &&
    snprintf(rest, sizeof(rest), "slaves/loop%d", map->loopno) <
      (int)sizeof(rest) &&
    crypt_sysfs_block_path(path, sizeof(path), node, rest) &&
    access(path, F_OK) == 0 &&
    loop_backing_identity(map->loopno, &dev, &ino) &&
    dev == map->dev && ino == map->ino;
}


bool crypt_swaps_slot(const char devname[], int limit, int *seqno)
{
  if (crypt_parse_mapper_path(devname, limit, seqno)) return true;

  char sysfs[64];
  return crypt_dm_sysfs_path(devname, sysfs, sizeof(sysfs)) &&
    crypt_read_mapper_name(sysfs, limit, seqno);
}
