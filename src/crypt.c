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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "crypt.h"


bool crypt_enabled = false;
char crypt_cipher[CRYPT_CIPHER_MAX] = CRYPT_DEFAULT_CIPHER;
int crypt_keysize = CRYPT_DEFAULT_KEYSIZE;
const char *crypt_sysfs_root = "/sys";


#ifndef NO_CONFIG
char *set_crypt(long long dummy)
{
  crypt_enabled = true;
  return NULL;
}

char *set_crypt_cipher(long long dummy)
{
  return crypt_cipher;
}

char *set_crypt_keysize(long long bits)
{
  crypt_keysize = (int)bits;
  return NULL;
}

bool crypt_check_config(void)
{
  if (!crypt_cipher_valid(crypt_cipher))
  {
    fputs("Configuration error: 'crypt_cipher': must start with a letter or "
	"digit, contain only letters, digits and ':_.-', and not name a null or "
	"ECB cipher\n", stderr);
    return false;
  }
  if (!crypt_keysize_valid(crypt_cipher, crypt_keysize))
  {
    fprintf(stderr,
	"Configuration error: 'crypt_keysize': %d bits is not usable with "
	"cipher '%s' (xts needs 256 or 512, other aes modes 128, 192 or 256)\n",
	crypt_keysize,
	crypt_cipher);
    return false;
  }
  return true;
}
#endif


/// Does the dash-separated cipher specification contain the given component?
static bool cipher_has_part(const char cipher[], const char part[])
{
  const size_t len = strlen(part);
  const char *p = cipher;
  for (;;)
  {
    const char *const dash = strchr(p, '-');
    const size_t n = dash ? (size_t)(dash - p) : strlen(p);
    if (n == len && strncmp(p, part, len) == 0) return true;
    if (!dash) return false;
    p = dash + 1;
  }
}


/// Does the cipher specification name a mode that provides no confidentiality?
/** "cipher_null" stores plaintext, and ECB leaves repeated blocks (zero pages,
 * repeated data) recognizable.  Neither is acceptable for swap.  Case is
 * ignored because the kernel's algorithm lookup is case-insensitive.
 */
static bool cipher_weak(const char cipher[])
{
  char low[CRYPT_CIPHER_MAX];
  size_t i;
  for (i = 0; cipher[i] && i < sizeof(low) - 1; ++i)
    low[i] = (cipher[i] >= 'A' && cipher[i] <= 'Z') ? cipher[i] + 32 : cipher[i];
  low[i] = '\0';
  return strstr(low, "null") != NULL || cipher_has_part(low, "ecb");
}


bool crypt_cipher_valid(const char cipher[])
{
  if (!cipher || !*cipher) return false;
  if (strlen(cipher) >= CRYPT_CIPHER_MAX) return false;

  for (const char *p = cipher; *p; ++p)
  {
    const char c = *p;
    const bool alnum =
      (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (alnum) continue;
    // Punctuation is fine, but not as the first character.
    if (p != cipher && (c == ':' || c == '_' || c == '.' || c == '-')) continue;
    return false;
  }
  return !cipher_weak(cipher);
}


bool crypt_keysize_valid(const char cipher[], long long bits)
{
  if (bits < CRYPT_KEYSIZE_MIN || bits > CRYPT_KEYSIZE_MAX) return false;
  if (bits % 8 != 0) return false;

  if (cipher_has_part(cipher, "xts")) return bits == 256 || bits == 512;

  // A bare "aes" is accepted by cryptsetup too (it defaults the mode).
  if (strcmp(cipher, "aes") == 0 || strncmp(cipher, "aes-", 4) == 0)
    return bits == 128 || bits == 192 || bits == 256;

  // Any other cipher is range-checked only.
  return true;
}


int crypt_keysize_bytes(long long bits)
{
  if (bits < CRYPT_KEYSIZE_MIN || bits > CRYPT_KEYSIZE_MAX || bits % 8 != 0)
    return 0;
  return (int)(bits / 8);
}


/// Format into buf; false if it did not fit or formatting failed
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static bool fmt(char buf[], size_t size, const char format[], ...)
{
  va_list ap;
  va_start(ap, format);
  const int n = vsnprintf(buf, size, format, ap);
  va_end(ap);
  return n >= 0 && (size_t)n < size;
}


bool crypt_mapper_name(char buf[], size_t size, int seqno)
{
  if (seqno < 0) return false;
  return fmt(buf, size, CRYPT_NAME_PREFIX "%d", seqno);
}


bool crypt_mapper_path(char buf[], size_t size, int seqno)
{
  if (seqno < 0) return false;
  return fmt(buf, size, CRYPT_MAPPER_DIR CRYPT_NAME_PREFIX "%d", seqno);
}


/// Parse a plain decimal number: digits only, no leading zeros, below limit
static bool parse_decimal(const char text[], int limit, int *result)
{
  if (!*text) return false;
  if (text[0] == '0' && text[1]) return false;

  long value = 0;
  for (const char *p = text; *p; ++p)
  {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + (*p - '0');
    if (value >= limit) return false;
  }
  *result = (int)value;
  return true;
}


bool crypt_parse_mapper_name(const char name[], int limit, int *seqno)
{
  const size_t plen = strlen(CRYPT_NAME_PREFIX);
  if (strncmp(name, CRYPT_NAME_PREFIX, plen) != 0) return false;
  return parse_decimal(name + plen, limit, seqno);
}


bool crypt_parse_mapper_path(const char path[], int limit, int *seqno)
{
  const size_t dlen = strlen(CRYPT_MAPPER_DIR);
  if (strncmp(path, CRYPT_MAPPER_DIR, dlen) != 0) return false;
  return crypt_parse_mapper_name(path + dlen, limit, seqno);
}


bool crypt_sysfs_block_path(char buf[], size_t size, const char node[],
    const char rest[])
{
  if (!node || !*node) return false;
  if (!rest || !*rest) return fmt(buf, size, "%s/block/%s", crypt_sysfs_root, node);
  return fmt(buf, size, "%s/block/%s/%s", crypt_sysfs_root, node, rest);
}


bool crypt_dm_node_number(const char devpath[], int *dmno)
{
  static const char prefix[] = "/dev/dm-";
  if (strncmp(devpath, prefix, sizeof(prefix)-1) != 0) return false;
  const char *const digits = devpath + sizeof(prefix)-1;

  // Plain decimal only; the kernel decides whether the device exists.
  if (!*digits || strlen(digits) > 9) return false;
  long value = 0;
  for (const char *p = digits; *p; ++p)
  {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + (*p - '0');
  }
  *dmno = (int)value;
  return true;
}


bool crypt_dm_sysfs_path(const char devpath[], char buf[], size_t size)
{
  int dmno;
  if (!crypt_dm_node_number(devpath, &dmno)) return false;

  char node[32];
  return fmt(node, sizeof(node), "dm-%d", dmno) &&
    crypt_sysfs_block_path(buf, size, node, "dm/name");
}


unsigned int crypt_loop_blocksize(unsigned long dio_align, unsigned long blksize,
    unsigned long pagesize)
{
  unsigned long want;
  if (dio_align)
  {
    /* Swap needs a logical block size no larger than a page, so an alignment
     * beyond that cannot be matched.  Return 0: the caller must refuse, since
     * block size 0 would make loop.c take the alignment and keep direct I/O.
     */
    if (dio_align > pagesize) return 0;
    want = dio_align;
  }
  else
  {
    // Only a preferred size: it can be large (striping), so it is not a need.
    want = blksize < pagesize ? blksize : pagesize;
  }

  unsigned long bs = 512;
  while (bs < want) bs <<= 1;
  return (unsigned int)bs;
}


bool crypt_loop_path(char buf[], size_t size, int loopno)
{
  if (loopno < 0) return false;
  return fmt(buf, size, "/dev/loop%d", loopno);
}


bool crypt_open_cmd(char buf[], size_t size, const char cipher[],
    long long bits, int seqno, int loopno)
{
  if (seqno < 0 || loopno < 0) return false;
  if (!crypt_cipher_valid(cipher) || !crypt_keysize_valid(cipher, bits))
    return false;

  return fmt(buf, size,
      "cryptsetup open --type plain --cipher '%s' --key-size %d "
      "--key-file /dev/urandom --keyfile-size %d --batch-mode "
      "/dev/loop%d " CRYPT_NAME_PREFIX "%d",
      cipher,
      (int)bits,
      crypt_keysize_bytes(bits),
      loopno,
      seqno);
}


bool crypt_close_cmd(char buf[], size_t size, int seqno)
{
  if (seqno < 0) return false;
  return fmt(buf, size, "cryptsetup close " CRYPT_NAME_PREFIX "%d", seqno);
}
