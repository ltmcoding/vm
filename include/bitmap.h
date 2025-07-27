#ifndef BITMAP_H
#define BITMAP_H

#include <Windows.h>
#include <intrin.h>

typedef struct {
    PULONG64 data;
    ULONG64 size_in_bits;
    ULONG64 unset_spaces;
} INTERLOCKED_BITMAP, *PINTERLOCKED_BITMAP;

extern PINTERLOCKED_BITMAP bitmap_create(ULONG64 size_in_bits);
extern VOID bitmap_destroy(PINTERLOCKED_BITMAP bitmap);

extern BOOLEAN bitmap_get_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 bit_index);
extern BOOLEAN bitmap_set_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 bit_index);
extern BOOLEAN bitmap_unset_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 bit_index);
extern BOOLEAN bitmap_flip_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 bit_index);

extern ULONG64 bitmap_get_chunk_value(PINTERLOCKED_BITMAP bitmap, ULONG64 chunk_index);
extern ULONG64 bitmap_set_chunk(PINTERLOCKED_BITMAP bitmap, ULONG64 chunk_index, ULONG64 new_value, ULONG64 expected_value);

extern ULONG64 bitmap_get_unset_spaces(PINTERLOCKED_BITMAP bitmap);
extern ULONG64 bitmap_get_set_spaces(PINTERLOCKED_BITMAP bitmap);

extern ULONG64 bitmap_search_for_set_bit(PINTERLOCKED_BITMAP bitmap,ULONG64 start_index,BOOLEAN unset_bit);
extern ULONG64 bitmap_search_for_unset_bit(PINTERLOCKED_BITMAP bitmap,ULONG64 start_index,BOOLEAN set_bit);

#endif // BITMAP_H
