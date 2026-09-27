/* Bounds regressions for the BSON reader used by GBA savestates. */
#include <assert.h>
#include <string.h>
#include "../savestate.c"

static void write_u32_at(u8 *p, u32 value)
{
  p[0] = (u8)value;
  p[1] = (u8)(value >> 8);
  p[2] = (u8)(value >> 16);
  p[3] = (u8)(value >> 24);
}

int main(void)
{
  u8 valid_i32[12] = {
    12, 0, 0, 0, BSON_TYPE_INT32, 'x', 0, 1, 0, 0, 0, 0
  };
  u8 valid_nested[13] = {
    13, 0, 0, 0, BSON_TYPE_DOC, 'd', 0,
    5, 0, 0, 0, 0, 0
  };
  u8 valid_binary[14] = {
    14, 0, 0, 0, BSON_TYPE_BIN, 'x', 0, 1, 0, 0, 0, 0, 0x5a, 0
  };
  u8 valid_array[13] = {
    13, 0, 0, 0, BSON_TYPE_INT32, '0', '0', 0, 1, 0, 0, 0, 0
  };
  u8 malformed_array[13] = {
    13, 0, 0, 0, BSON_TYPE_INT32, '0', '1', 0, 1, 0, 0, 0, 0
  };
  u8 unterminated_key[12];
  u8 child_overrun[13];
  u8 binary_overrun[14];

  assert(bson_validate_document(valid_i32, sizeof(valid_i32), 0, false));
  assert(bson_validate_document(valid_nested, sizeof(valid_nested), 0, false));
  assert(bson_validate_document(valid_binary, sizeof(valid_binary), 0, false));
  assert(bson_validate_document(valid_array, sizeof(valid_array), 0, true));
  assert(!bson_validate_document(malformed_array, sizeof(malformed_array), 0, true));
  assert(!bson_validate_document(valid_i32, sizeof(valid_i32) - 1, 0, false));

  memset(unterminated_key, 'a', sizeof(unterminated_key));
  write_u32_at(unterminated_key, sizeof(unterminated_key));
  unterminated_key[4] = BSON_TYPE_INT32;
  unterminated_key[5] = 'x';
  unterminated_key[11] = 0; /* only the document terminator is present */
  assert(!bson_validate_document(unterminated_key,
                                sizeof(unterminated_key), 0, false));

  memcpy(child_overrun, valid_nested, sizeof(child_overrun));
  write_u32_at(child_overrun + 7, 0x1000);
  assert(!bson_validate_document(child_overrun, sizeof(child_overrun), 0, false));

  memset(binary_overrun, 0, sizeof(binary_overrun));
  write_u32_at(binary_overrun, sizeof(binary_overrun));
  binary_overrun[4] = BSON_TYPE_BIN;
  binary_overrun[5] = 'b';
  binary_overrun[6] = 0;
  write_u32_at(binary_overrun + 7, 8); /* only one byte fits before terminator */
  binary_overrun[11] = 0; /* subtype */
  binary_overrun[12] = 0xAA;
  binary_overrun[13] = 0;
  assert(!bson_validate_document(binary_overrun, sizeof(binary_overrun), 0, false));

  assert(!bson_validate_document(valid_nested, sizeof(valid_nested), 17, false));
  return 0;
}
