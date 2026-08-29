// Host-side unit tests for the goodix53x5 wire-protocol helpers.
//
// These do NOT need the sensor, and they compile the real
// drivers/goodix53x5/goodix53x5-proto.c translation unit - tests/shim provides
// a stand-in for libfprint's internal drivers_api.h so no configured libfprint
// tree is required (see tests/shim/drivers_api.h).
//
// Scope: goodix_proto_validate_checksum(). The 0x88 trailer is a no-checksum
// marker, and which message classes rely on it is NOT obvious from the code -
// the comment in proto.c calls it a handshake marker, which is wrong. It was
// determined empirically on a 27c6:5395 by logging data[0] for every received
// frame carrying a 0x88 trailer, across device open and a full verify:
//
//   ZZPROBE 0x88 trailer: cmd_byte=0x20 category=0x2 len=14309
//                         computed=0x1c additive_ok=0
//
// That is the sensor image frame (category 0x2, the reply to the image request
// in goodix53x5-commands.c), 14309 bytes, and its additive checksum does NOT
// match - the device genuinely relies on the marker. The GTLS handshake, which
// travels as category 0xD MCU messages, does not: it checksums normally.
//
// So narrowing the marker to a "handshake category" rejects every image the
// sensor sends and breaks enroll and verify completely. These tests pin that
// down so the narrowing cannot be re-introduced silently.
//
// Build and run from the repo root:
//
//   gcc -std=gnu99 -o /tmp/test_goodix_proto tests/test_goodix53x5_proto.c
//     drivers/goodix53x5/goodix53x5-proto.c -Itests/shim -Idrivers/goodix53x5
//     $(pkg-config --cflags --libs glib-2.0)
//   /tmp/test_goodix_proto
//
// (join those continuation lines into one command)

#include "goodix53x5-proto.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
      if (!(cond))                                                             \
        {                                                                      \
          printf ("  FAIL: %s\n", msg);                                        \
          failures++;                                                          \
        }                                                                      \
      else                                                                     \
        {                                                                      \
          printf ("  ok:   %s\n", msg);                                        \
        }                                                                      \
  } while (0)

static GLogWriterOutput
silence_logs (GLogLevelFlags log_level, const GLogField *fields,
              gsize n_fields, gpointer user_data)
{
  (void) log_level; (void) fields; (void) n_fields; (void) user_data;
  return G_LOG_WRITER_HANDLED;
}

/* Fill in the trailing additive checksum the device would compute. */
static void
set_additive_checksum (guint8 *msg, gsize len)
{
  guint sum = 0;

  for (gsize i = 0; i < len - 1; i++)
    sum += msg[i];

  msg[len - 1] = (guint8) ((0xAA - sum) & 0xFF);
}

static void
test_valid_additive_checksum_accepted (void)
{
  guint8 msg[8] = { 0xB0, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00 };

  set_additive_checksum (msg, sizeof (msg));
  CHECK (goodix_proto_validate_checksum (msg, sizeof (msg)),
         "valid additive checksum accepted");
}

static void
test_bad_additive_checksum_rejected (void)
{
  guint8 msg[8] = { 0xB0, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00 };

  set_additive_checksum (msg, sizeof (msg));
  msg[sizeof (msg) - 1] ^= 0xFF;
  CHECK (!goodix_proto_validate_checksum (msg, sizeof (msg)),
         "corrupted additive checksum rejected");
}

/* The regression case. A category 0x2 image reply carries a literal 0x88
 * trailer and no valid additive checksum. Rejecting it kills every scan. */
static void
test_image_reply_no_checksum_marker_accepted (void)
{
  guint8 msg[8] = { 0x20, 0x05, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0x88 };
  guint sum = 0;

  for (gsize i = 0; i < sizeof (msg) - 1; i++)
    sum += msg[i];

  CHECK (((0xAA - sum) & 0xFF) != 0x88,
         "test fixture really does have a mismatched additive checksum");
  CHECK (goodix_proto_validate_checksum (msg, sizeof (msg)),
         "category 0x2 image reply with 0x88 marker accepted");
}

/* The same marker on the categories the driver also receives. Only category
 * 0x2 was observed using it, but nothing in the protocol scopes it, so a
 * change that narrows it by category is a change in device behaviour and
 * needs sensor evidence rather than a plausible-looking guess. */
static void
test_no_checksum_marker_not_scoped_by_category (void)
{
  const guint8 categories[] = { 0x00, 0x20, 0x30, 0xB0, 0xD0, 0xE0 };
  gboolean all_accepted = TRUE;

  for (gsize i = 0; i < G_N_ELEMENTS (categories); i++)
    {
      guint8 msg[8] = { 0x00, 0x05, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0x88 };

      msg[0] = categories[i];
      if (!goodix_proto_validate_checksum (msg, sizeof (msg)))
        all_accepted = FALSE;
    }

  CHECK (all_accepted, "0x88 marker honored regardless of message category");
}

static void
test_short_messages_rejected (void)
{
  guint8 msg[3] = { 0xB0, 0x00, 0x88 };

  CHECK (!goodix_proto_validate_checksum (msg, sizeof (msg)),
         "message shorter than the 4-byte header rejected");
}

int
main (void)
{
  g_log_set_writer_func (silence_logs, NULL, NULL);

  test_valid_additive_checksum_accepted ();
  test_bad_additive_checksum_rejected ();
  test_image_reply_no_checksum_marker_accepted ();
  test_no_checksum_marker_not_scoped_by_category ();
  test_short_messages_rejected ();

  if (failures == 0)
    {
      printf ("\nALL TESTS PASSED\n");
      return 0;
    }
  printf ("\n%d TEST(S) FAILED\n", failures);
  return 1;
}
