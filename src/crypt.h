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
#ifndef SWAPSPACE_CRYPT_H
#define SWAPSPACE_CRYPT_H

#include <stdbool.h>
#include <stddef.h>

/* Optional per-file encryption ("crypt" mode).
 *
 * This header declares the configuration and the pure helpers: validation and
 * name/command-line formatting that touch no system state, so they can be unit
 * tested without privileges.  The parts that talk to the kernel and to
 * cryptsetup live in cryptdev.c.
 */

/// Size of the cipher string buffer, including the terminating NUL
#define CRYPT_CIPHER_MAX 64

#define CRYPT_DEFAULT_CIPHER "aes-xts-plain64"
#define CRYPT_DEFAULT_KEYSIZE 512
#define CRYPT_KEYSIZE_MIN 128
#define CRYPT_KEYSIZE_MAX 1024

/// Is crypt mode switched on?  Always false in the unconfigurable build.
extern bool crypt_enabled;
/// dm-crypt cipher specification, e.g. "aes-xts-plain64"
extern char crypt_cipher[CRYPT_CIPHER_MAX];
/// Key size in bits
extern int crypt_keysize;

#ifndef NO_CONFIG
char *set_crypt(long long dummy);
char *set_crypt_cipher(long long dummy);
char *set_crypt_keysize(long long bits);

/// Verify the crypt configuration; prints a message to stderr on failure
bool crypt_check_config(void);
#endif

/// Is this an acceptable cipher string?
/** The string ends up on a shell command line, so it is restricted to a
 * whitelist: it must start with a letter or digit (so it cannot look like an
 * option), and may contain only letters, digits and any of ":_.-".  It must be
 * non-empty and shorter than CRYPT_CIPHER_MAX.  Ciphers that give no
 * confidentiality are refused: anything with "null" in its name (cipher_null
 * stores plaintext) and the ECB chain mode (repeated blocks stay visible), in
 * any letter case.
 */
bool crypt_cipher_valid(const char cipher[]);

/// Is this key size (in bits) usable with the given cipher?
/** The size must lie in [CRYPT_KEYSIZE_MIN, CRYPT_KEYSIZE_MAX] and be a
 * multiple of 8.  XTS modes take two keys, so their size must be 256 or 512;
 * "aes" and the other "aes-" modes take 128, 192 or 256.  Any other cipher
 * (serpent, twofish, ...) is only range-checked here: cryptsetup or the kernel
 * refuses an unsuitable size when the mapping is created, and crypt mode then
 * fails closed with a logged error instead of rejecting the configuration up
 * front.
 */
bool crypt_keysize_valid(const char cipher[], long long bits);

/// Number of key bytes for a key size in bits, or 0 if the size is unusable
int crypt_keysize_bytes(long long bits);

/// Prefix of the device-mapper names we create: "swapspace-<seqno>"
#define CRYPT_NAME_PREFIX "swapspace-"
/// Directory holding device-mapper nodes
#define CRYPT_MAPPER_DIR "/dev/mapper/"

/* The formatting helpers write to a caller-provided buffer and return false,
 * leaving the contents unspecified, if the result (including its NUL) does not
 * fit or an argument is out of range.
 */

/// Device-mapper name for swapfile seqno: "swapspace-<seqno>"
bool crypt_mapper_name(char buf[], size_t size, int seqno);

/// Device node for swapfile seqno: "/dev/mapper/swapspace-<seqno>"
bool crypt_mapper_path(char buf[], size_t size, int seqno);

/// Recognize "swapspace-<seqno>" with 0 <= seqno < limit
/** The number must be plain decimal digits without sign or leading zeros, so
 * that only names we generated are recognized.
 */
bool crypt_parse_mapper_name(const char name[], int limit, int *seqno);

/// Recognize "/dev/mapper/swapspace-<seqno>"; see crypt_parse_mapper_name()
bool crypt_parse_mapper_path(const char path[], int limit, int *seqno);

/// Root of the sysfs mount; "/sys" unless a test points it at a fake tree
extern const char *crypt_sysfs_root;

/// "<sysfs>/block/<node>" or "<sysfs>/block/<node>/<rest>" (rest may be NULL)
bool crypt_sysfs_block_path(char buf[], size_t size, const char node[],
    const char rest[]);

/// Number X of a device node name of the form "/dev/dm-<X>" (plain decimal)
bool crypt_dm_node_number(const char devpath[], int *dmno);

/// Sysfs file holding the mapper name of a device node such as "/dev/dm-3"
/** The result is "/sys/block/dm-3/dm/name".  This is how /proc/swaps entries
 * naming a dm node are traced back to "swapspace-<seqno>".
 * @return false if devpath is not of the form "/dev/dm-<digits>"
 */
bool crypt_dm_sysfs_path(const char devpath[], char buf[], size_t size);

/// Logical block size to give a loop device with direct I/O, or 0 for none
/** Direct I/O needs the loop device's logical block size to be at least the
 * backing file's direct I/O offset alignment ("dio_align", from statx, 0 if
 * unknown): a 4Kn disk needs 4096, while the loop default is 512.  Without that
 * answer the filesystem's preferred I/O size "blksize" is used instead, capped
 * at the page size because it can be much larger than any alignment need.
 * The result is a power of two from 512 up to the page size.
 *
 * Mainline allows loop block sizes up to 64 KiB, but swap does I/O in pages,
 * which a device with larger logical blocks cannot serve, so nothing above the
 * page size is ever requested.  If the filesystem demands a larger alignment,
 * the result is 0 and the caller must refuse: handing 0 to the kernel would
 * make loop.c use that alignment as the block size and keep direct I/O.
 */
unsigned int crypt_loop_blocksize(unsigned long dio_align, unsigned long blksize,
    unsigned long pagesize);

/// Loop device node: "/dev/loop<loopno>"
bool crypt_loop_path(char buf[], size_t size, int loopno);

/// Compose the cryptsetup command that maps a loop device with a random key
/** The key is read from /dev/urandom by cryptsetup (plain mode with a key file
 * uses the file content directly as the key).  The cipher and key size are
 * validated again here.
 *
 * "--keyfile-size" repeats the key size in bytes.  Without --hash, cryptsetup
 * ignores it for a key file and reads exactly --key-size bits (checked in
 * action_open_plain() of cryptsetup 2.x, src/cryptsetup.c); the flag is kept
 * so that the command states what is meant and so that a cryptsetup that does
 * honour it cannot read more than the key size.  --batch-mode keeps it from
 * prompting.
 */
bool crypt_open_cmd(char buf[], size_t size, const char cipher[],
    long long bits, int seqno, int loopno);

/// Compose the cryptsetup command that removes the mapping for swapfile seqno
bool crypt_close_cmd(char buf[], size_t size, int seqno);

#endif
