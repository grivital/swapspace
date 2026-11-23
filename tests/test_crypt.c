/* Unit tests for the pure helpers in src/crypt.c.
 *
 * Like test_calc, this includes the source file so the tests exercise exactly
 * the code that goes into the daemon.
 */
#include <stdio.h>

#include "../src/crypt.c"

static int failures = 0;

#define CHECK(expr) \
  do \
  { \
    if (!(expr)) \
    { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
      ++failures; \
    } \
  } while (0)


int main(void)
{
  // Defaults: off, and the defaults themselves must be acceptable.
  CHECK(!crypt_enabled);
  CHECK(crypt_cipher_valid(crypt_cipher));
  CHECK(crypt_keysize_valid(crypt_cipher, crypt_keysize));

  // Cipher whitelist.
  CHECK(crypt_cipher_valid("aes-xts-plain64"));
  CHECK(crypt_cipher_valid("aes-cbc-essiv:sha256"));
  CHECK(crypt_cipher_valid("serpent-xts-plain64"));
  CHECK(crypt_cipher_valid("a"));
  CHECK(crypt_cipher_valid("Aes_XTS.plain-64:x"));
  CHECK(!crypt_cipher_valid(NULL));
  CHECK(!crypt_cipher_valid(""));
  CHECK(!crypt_cipher_valid("-aes"));       // would look like an option
  CHECK(!crypt_cipher_valid(":aes"));
  CHECK(!crypt_cipher_valid("aes xts"));
  CHECK(!crypt_cipher_valid("aes\txts"));
  CHECK(!crypt_cipher_valid("aes\nxts"));
  CHECK(!crypt_cipher_valid("aes;reboot"));
  CHECK(!crypt_cipher_valid("aes&&id"));
  CHECK(!crypt_cipher_valid("aes|id"));
  CHECK(!crypt_cipher_valid("aes$(id)"));
  CHECK(!crypt_cipher_valid("aes`id`"));
  CHECK(!crypt_cipher_valid("aes'id"));
  CHECK(!crypt_cipher_valid("aes\"id"));
  CHECK(!crypt_cipher_valid("aes\\id"));
  CHECK(!crypt_cipher_valid("aes>x"));
  CHECK(!crypt_cipher_valid("aes<x"));
  CHECK(!crypt_cipher_valid("aes*"));
  CHECK(!crypt_cipher_valid("aes#x"));
  CHECK(!crypt_cipher_valid("capi:xts(aes)-plain64"));
  CHECK(!crypt_cipher_valid("aes/xts"));
  CHECK(!crypt_cipher_valid("aes\x80"));   // non-ASCII byte

  // Ciphers that protect nothing are refused, in any letter case: null ciphers
  // and the ECB mode (which keeps repeated blocks recognizable).
  CHECK(!crypt_cipher_valid("cipher_null-ecb"));
  CHECK(!crypt_cipher_valid("cipher_null"));
  CHECK(!crypt_cipher_valid("cipher_null-cbc-plain64"));
  CHECK(!crypt_cipher_valid("CIPHER_NULL-ecb"));
  CHECK(!crypt_cipher_valid("aes-null-plain64"));
  CHECK(!crypt_cipher_valid("aes-ecb"));
  CHECK(!crypt_cipher_valid("aes-ECB-plain"));
  CHECK(!crypt_cipher_valid("serpent-ecb"));
  CHECK(crypt_cipher_valid("aes-cbc-essiv:sha256"));	// "ecb" must be a whole part
  CHECK(crypt_cipher_valid("aes-xts-plain64"));
  {
    char cmd[256];
    CHECK(!crypt_open_cmd(cmd, sizeof(cmd), "cipher_null-ecb", 256, 1, 1));
  }

  // Length limit: 63 characters fit, 64 do not.
  char longname[CRYPT_CIPHER_MAX + 1];
  memset(longname, 'a', sizeof(longname));
  longname[CRYPT_CIPHER_MAX - 1] = '\0';
  CHECK(crypt_cipher_valid(longname));
  longname[CRYPT_CIPHER_MAX - 1] = 'a';
  longname[CRYPT_CIPHER_MAX] = '\0';
  CHECK(!crypt_cipher_valid(longname));

  // Key sizes: range and multiples of 8.
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", 120));
  CHECK(crypt_keysize_valid("serpent-cbc-plain", 128));
  CHECK(crypt_keysize_valid("serpent-cbc-plain", 136));
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", 129));
  CHECK(crypt_keysize_valid("serpent-cbc-plain", 1024));
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", 1032));
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", 0));
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", -256));

  // XTS takes two keys: 256 or 512 only.
  CHECK(crypt_keysize_valid("aes-xts-plain64", 256));
  CHECK(crypt_keysize_valid("aes-xts-plain64", 512));
  CHECK(!crypt_keysize_valid("aes-xts-plain64", 128));
  CHECK(!crypt_keysize_valid("aes-xts-plain64", 192));
  CHECK(!crypt_keysize_valid("aes-xts-plain64", 384));
  CHECK(!crypt_keysize_valid("aes-xts-plain64", 1024));
  CHECK(crypt_keysize_valid("serpent-xts-plain64", 512));
  CHECK(!crypt_keysize_valid("serpent-xts-plain64", 128));
  // "xts" must be a whole component, not a substring.
  CHECK(crypt_keysize_valid("foo-boxtsx-plain", 128));

  // Other AES modes: 128, 192 or 256.
  CHECK(crypt_keysize_valid("aes-cbc-essiv:sha256", 128));
  CHECK(crypt_keysize_valid("aes-cbc-essiv:sha256", 192));
  CHECK(crypt_keysize_valid("aes-cbc-essiv:sha256", 256));
  CHECK(!crypt_keysize_valid("aes-cbc-essiv:sha256", 512));
  CHECK(!crypt_keysize_valid("aes-cbc-essiv:sha256", 136));

  // A bare "aes" is checked like the other AES modes; other ciphers only by
  // range, which is documented rather than enforced.
  CHECK(crypt_keysize_valid("aes", 128));
  CHECK(crypt_keysize_valid("aes", 256));
  CHECK(!crypt_keysize_valid("aes", 512));
  CHECK(!crypt_keysize_valid("aes", 136));
  CHECK(crypt_keysize_valid("aesx", 512));
  CHECK(crypt_keysize_valid("twofish-cbc-plain", 512));

  // Bits to bytes.
  CHECK(crypt_keysize_bytes(128) == 16);
  CHECK(crypt_keysize_bytes(256) == 32);
  CHECK(crypt_keysize_bytes(512) == 64);
  CHECK(crypt_keysize_bytes(1024) == 128);
  CHECK(crypt_keysize_bytes(127) == 0);
  CHECK(crypt_keysize_bytes(1025) == 0);
  CHECK(crypt_keysize_bytes(0) == 0);
  CHECK(crypt_keysize_bytes(-64) == 0);
  CHECK(crypt_keysize_bytes(130) == 0);

  // Name formatting, including exact-fit and one-short buffers.  A guard
  // region after the buffer must never be touched.
  char guarded[64];
  memset(guarded, 'G', sizeof(guarded));
#define GUARD_OK(from) \
  ({ bool ok = true; \
     for (size_t g = (from); g < sizeof(guarded); ++g) \
       if (guarded[g] != 'G') ok = false; \
     ok; })

  CHECK(crypt_mapper_name(guarded, 13, 12));
  CHECK(strcmp(guarded, "swapspace-12") == 0);
  CHECK(GUARD_OK(13));
  memset(guarded, 'G', sizeof(guarded));
  CHECK(!crypt_mapper_name(guarded, 12, 12));
  CHECK(GUARD_OK(12));
  memset(guarded, 'G', sizeof(guarded));
  CHECK(!crypt_mapper_name(guarded, 0, 12));
  CHECK(GUARD_OK(0));
  CHECK(crypt_mapper_name(guarded, sizeof(guarded), 0));
  CHECK(strcmp(guarded, "swapspace-0") == 0);
  CHECK(!crypt_mapper_name(guarded, sizeof(guarded), -1));

  memset(guarded, 'G', sizeof(guarded));
  CHECK(crypt_mapper_path(guarded, 25, 31));
  CHECK(strcmp(guarded, "/dev/mapper/swapspace-31") == 0);
  CHECK(GUARD_OK(25));
  memset(guarded, 'G', sizeof(guarded));
  CHECK(!crypt_mapper_path(guarded, 24, 31));
  CHECK(GUARD_OK(24));
  CHECK(!crypt_mapper_path(guarded, sizeof(guarded), -5));

  CHECK(crypt_loop_path(guarded, sizeof(guarded), 7));
  CHECK(strcmp(guarded, "/dev/loop7") == 0);
  CHECK(!crypt_loop_path(guarded, 10, 7));
  CHECK(crypt_loop_path(guarded, 11, 7));
  CHECK(!crypt_loop_path(guarded, sizeof(guarded), -1));

  // Parsing names back.  Only names we generate are recognized.
  int n = -1;
  CHECK(crypt_parse_mapper_name("swapspace-0", 32, &n) && n == 0);
  CHECK(crypt_parse_mapper_name("swapspace-31", 32, &n) && n == 31);
  CHECK(!crypt_parse_mapper_name("swapspace-32", 32, &n));
  CHECK(!crypt_parse_mapper_name("swapspace-99999999999999999999", 32, &n));
  CHECK(!crypt_parse_mapper_name("swapspace-", 32, &n));
  CHECK(!crypt_parse_mapper_name("swapspace-007", 32, &n));
  CHECK(!crypt_parse_mapper_name("swapspace--1", 32, &n));
  CHECK(!crypt_parse_mapper_name("swapspace-+1", 32, &n));
  CHECK(!crypt_parse_mapper_name("swapspace-1x", 32, &n));
  CHECK(!crypt_parse_mapper_name("swapspace-1 ", 32, &n));
  CHECK(!crypt_parse_mapper_name("swapspace1", 32, &n));
  CHECK(!crypt_parse_mapper_name("Swapspace-1", 32, &n));
  CHECK(!crypt_parse_mapper_name("cryptswap-1", 32, &n));
  CHECK(!crypt_parse_mapper_name("", 32, &n));
  CHECK(crypt_parse_mapper_path("/dev/mapper/swapspace-5", 32, &n) && n == 5);
  CHECK(!crypt_parse_mapper_path("/dev/mapper/swapspace-x", 32, &n));
  CHECK(!crypt_parse_mapper_path("/dev/mapper2/swapspace-5", 32, &n));
  CHECK(!crypt_parse_mapper_path("swapspace-5", 32, &n));
  CHECK(!crypt_parse_mapper_path("/dev/dm-5", 32, &n));
  CHECK(!crypt_parse_mapper_path("/dev/mapper/", 32, &n));

  // Round trip for every slot.
  for (int i = 0; i < 32; ++i)
  {
    char path[64];
    CHECK(crypt_mapper_path(path, sizeof(path), i));
    n = -1;
    CHECK(crypt_parse_mapper_path(path, 32, &n) && n == i);
  }

  // dm node to sysfs name file.
  char sys[64];
  CHECK(crypt_dm_sysfs_path("/dev/dm-3", sys, sizeof(sys)));
  CHECK(strcmp(sys, "/sys/block/dm-3/dm/name") == 0);
  CHECK(crypt_dm_sysfs_path("/dev/dm-127", sys, sizeof(sys)));
  CHECK(strcmp(sys, "/sys/block/dm-127/dm/name") == 0);
  CHECK(!crypt_dm_sysfs_path("/dev/dm-", sys, sizeof(sys)));
  CHECK(!crypt_dm_sysfs_path("/dev/dm-3/../../x", sys, sizeof(sys)));
  CHECK(!crypt_dm_sysfs_path("/dev/dm-a", sys, sizeof(sys)));
  CHECK(!crypt_dm_sysfs_path("/dev/sda1", sys, sizeof(sys)));
  CHECK(!crypt_dm_sysfs_path("/dev/mapper/swapspace-1", sys, sizeof(sys)));
  CHECK(crypt_dm_sysfs_path("/dev/dm-3", sys, 24));
  CHECK(!crypt_dm_sysfs_path("/dev/dm-3", sys, 23));

  // The sysfs root is overridable, for tests that use a fake tree.
  crypt_sysfs_root = "/fake";
  CHECK(crypt_dm_sysfs_path("/dev/dm-3", sys, sizeof(sys)));
  CHECK(strcmp(sys, "/fake/block/dm-3/dm/name") == 0);
  CHECK(crypt_sysfs_block_path(sys, sizeof(sys), "loop4", "loop/backing_file"));
  CHECK(strcmp(sys, "/fake/block/loop4/loop/backing_file") == 0);
  CHECK(crypt_sysfs_block_path(sys, sizeof(sys), "dm-1", NULL));
  CHECK(strcmp(sys, "/fake/block/dm-1") == 0);
  CHECK(!crypt_sysfs_block_path(sys, sizeof(sys), "", "x"));
  CHECK(!crypt_sysfs_block_path(sys, 12, "loop4", "x"));
  crypt_sysfs_root = "/sys";

  // dm node numbers: plain decimal only.
  int dmno = -1;
  CHECK(crypt_dm_node_number("/dev/dm-0", &dmno) && dmno == 0);
  CHECK(crypt_dm_node_number("/dev/dm-123456789", &dmno) && dmno == 123456789);
  CHECK(!crypt_dm_node_number("/dev/dm-1234567890", &dmno));
  CHECK(!crypt_dm_node_number("/dev/dm-1x", &dmno));
  CHECK(!crypt_dm_node_number("/dev/dm--1", &dmno));
  CHECK(!crypt_dm_node_number("/dev/dm-", &dmno));

  // Loop block size: power of two from 512 to the page size.  The direct I/O
  // alignment, when known, decides; the preferred size only as a fallback.
  CHECK(crypt_loop_blocksize(0, 0, 4096) == 512);
  CHECK(crypt_loop_blocksize(512, 4096, 4096) == 512);	// alignment wins
  CHECK(crypt_loop_blocksize(513, 0, 4096) == 1024);
  CHECK(crypt_loop_blocksize(4096, 512, 4096) == 4096);
  CHECK(crypt_loop_blocksize(4096, 0, 16384) == 4096);
  CHECK(crypt_loop_blocksize(5000, 0, 16384) == 8192);
  CHECK(crypt_loop_blocksize(16384, 0, 16384) == 16384);
  CHECK(crypt_loop_blocksize(0, 4096, 4096) == 4096);	// no alignment known
  CHECK(crypt_loop_blocksize(0, 65536, 4096) == 4096);	// capped
  // An alignment beyond the page size cannot be met: no size, the caller refuses.
  CHECK(crypt_loop_blocksize(8192, 0, 4096) == 0);
  CHECK(crypt_loop_blocksize(65536, 4096, 16384) == 0);

  // The cryptsetup command lines.
  char cmd[512];
  CHECK(crypt_open_cmd(cmd, sizeof(cmd), "aes-xts-plain64", 512, 3, 9));
  CHECK(strcmp(cmd,
        "cryptsetup open --type plain --cipher 'aes-xts-plain64' "
        "--key-size 512 --key-file /dev/urandom --keyfile-size 64 "
        "--batch-mode /dev/loop9 swapspace-3") == 0);
  CHECK(crypt_open_cmd(cmd, sizeof(cmd), "aes-cbc-essiv:sha256", 256, 0, 0));
  CHECK(strstr(cmd, "--keyfile-size 32 ") != NULL);
  CHECK(strstr(cmd, "'aes-cbc-essiv:sha256'") != NULL);
  CHECK(!crypt_open_cmd(cmd, sizeof(cmd), "aes-xts-plain64;id", 512, 3, 9));
  CHECK(!crypt_open_cmd(cmd, sizeof(cmd), "aes-xts-plain64", 128, 3, 9));
  CHECK(!crypt_open_cmd(cmd, sizeof(cmd), "aes-xts-plain64", 512, -1, 9));
  CHECK(!crypt_open_cmd(cmd, sizeof(cmd), "aes-xts-plain64", 512, 3, -1));

  // The command must fit exactly: find its length, then try one byte less.
  crypt_open_cmd(cmd, sizeof(cmd), "aes-xts-plain64", 512, 3, 9);
  const size_t cmdlen = strlen(cmd);
  char cmdbuf[512];
  memset(cmdbuf, 'G', sizeof(cmdbuf));
  CHECK(crypt_open_cmd(cmdbuf, cmdlen + 1, "aes-xts-plain64", 512, 3, 9));
  CHECK(strcmp(cmdbuf, cmd) == 0);
  CHECK(cmdbuf[cmdlen + 1] == 'G');
  memset(cmdbuf, 'G', sizeof(cmdbuf));
  CHECK(!crypt_open_cmd(cmdbuf, cmdlen, "aes-xts-plain64", 512, 3, 9));
  CHECK(cmdbuf[cmdlen] == 'G');
  CHECK(!crypt_open_cmd(cmdbuf, 0, "aes-xts-plain64", 512, 3, 9));

  CHECK(crypt_close_cmd(cmd, sizeof(cmd), 17));
  CHECK(strcmp(cmd, "cryptsetup close swapspace-17") == 0);
  CHECK(crypt_close_cmd(cmdbuf, 30, 17));
  CHECK(!crypt_close_cmd(cmdbuf, 29, 17));
  CHECK(!crypt_close_cmd(cmd, sizeof(cmd), -1));

  // Setters, as the option table drives them.
  CHECK(set_crypt(0) == NULL && crypt_enabled);
  CHECK(set_crypt_cipher(0) == crypt_cipher);
  CHECK(set_crypt_keysize(256) == NULL && crypt_keysize == 256);
  CHECK(crypt_check_config());
  strcpy(crypt_cipher, "aes;id");
  CHECK(!crypt_check_config());
  strcpy(crypt_cipher, "aes-xts-plain64");
  set_crypt_keysize(192);
  CHECK(!crypt_check_config());

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
