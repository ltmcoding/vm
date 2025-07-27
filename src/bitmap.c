#include "bitmap.h" // Include the header file we just defined
#include <stdlib.h> // For malloc, free

static ULONG64 bitmap_get_chunk_index(ULONG64 bit_index) {
    return bit_index / 64ULL;
}

static ULONG bitmap_get_bit_offset_in_chunk(ULONG64 bit_index) {
    return (ULONG)(bit_index % 64ULL);
}

static BOOLEAN bitmap_interlocked_bit_test_and_set(PULONG64 target_chunk, ULONG offset) {
    return _interlockedbittestandset64((volatile LONG64*)target_chunk, offset);
}

static BOOLEAN bitmap_interlocked_bit_test_and_reset(PULONG64 target_chunk, ULONG offset) {
    return _interlockedbittestandreset64((volatile LONG64*)target_chunk, offset);
}

static BOOLEAN bitmap_get_bit_from_value(ULONG64 chunk_value, ULONG offset) {
    return (BOOLEAN)((chunk_value >> offset) & 1ULL);
}

PINTERLOCKED_BITMAP bitmap_create(ULONG64 size_in_bits) {
    if (size_in_bits == 0) {
        return NULL;
    }

    PINTERLOCKED_BITMAP bitmap = (PINTERLOCKED_BITMAP)malloc(sizeof(INTERLOCKED_BITMAP));
    if (bitmap == NULL) {
        return NULL;
    }

    ULONG64 size_in_chunks = (size_in_bits + 63ULL) / 64ULL;
    bitmap->data = (PULONG64)malloc(size_in_chunks * sizeof(ULONG64));
    if (bitmap->data == NULL) {
        free(bitmap);
        return NULL;
    }

    memset(bitmap->data, 0, (size_t)(size_in_chunks * sizeof(ULONG64)));
    bitmap->size_in_bits = size_in_bits;
    bitmap->unset_spaces = (LONG64)size_in_bits;

    return bitmap;
}

VOID bitmap_destroy(PINTERLOCKED_BITMAP bitmap) {
    if (bitmap == NULL) {
        return;
    }
    if (bitmap->data != NULL) {
        free(bitmap->data);
    }
    free(bitmap);
}

// This function returns the value of a specific bit in the bitmap.
BOOLEAN bitmap_get_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 bit_index) {
    if (bitmap == NULL || bit_index >= bitmap->size_in_bits) {
        return FALSE;
    }

    ULONG64 chunk_index = bitmap_get_chunk_index(bit_index);
    ULONG offset = bitmap_get_bit_offset_in_chunk(bit_index);

    ULONG64 chunk_value = bitmap->data[chunk_index];

    return bitmap_get_bit_from_value(chunk_value, offset);
}

// This function sets a bit in the bitmap and returns whether it was previously set or not.
BOOLEAN bitmap_set_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 bit_index) {
    if (bitmap == NULL || bit_index >= bitmap->size_in_bits) {
        return FALSE;
    }

    ULONG64 chunk_index = bitmap_get_chunk_index(bit_index);
    ULONG offset = bitmap_get_bit_offset_in_chunk(bit_index);

    // Atomically set the bit and get its previous state
    BOOLEAN was_set = bitmap_interlocked_bit_test_and_set(&bitmap->data[chunk_index], offset);

    // If the bit was previously unset, we have now taken a space
    if (!was_set) {
        InterlockedDecrement64((volatile LONG64*)&bitmap->unset_spaces);
    }

    return was_set;
}

// This function unsets a bit in the bitmap and returns whether it was previously set or not.
BOOLEAN bitmap_unset_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 bit_index) {
    if (bitmap == NULL || bit_index >= bitmap->size_in_bits) {
        // _ASSERT(FALSE && "Invalid bitmap or bit_index for bitmap_unset_bit");
        return FALSE;
    }

    ULONG64 chunk_index = bitmap_get_chunk_index(bit_index);
    ULONG offset = bitmap_get_bit_offset_in_chunk(bit_index);

    // Atomically unset the bit and get its previous state
    BOOLEAN was_set = bitmap_interlocked_bit_test_and_reset(&bitmap->data[chunk_index], offset);

    // If the bit was previously set, we have now freed a space
    if (was_set) {
        InterlockedIncrement64((volatile LONG64*)&bitmap->unset_spaces);
    }

    return was_set;
}

ULONG64 bitmap_get_chunk_value(PINTERLOCKED_BITMAP bitmap, ULONG64 chunk_index) {
    if (bitmap == NULL) {
        // _ASSERT(FALSE && "Invalid bitmap for bitmap_get_chunk_value");
        return 0ULL;
    }
    ULONG64 size_in_chunks = (bitmap->size_in_bits + 63ULL) / 64ULL;
    if (chunk_index >= size_in_chunks) {
        // _ASSERT(FALSE && "Invalid chunk_index for bitmap_get_chunk_value");
        return 0ULL;
    }

    // Direct read; atomic on 64-bit systems
    return bitmap->data[chunk_index];
}

ULONG64 bitmap_set_chunk(PINTERLOCKED_BITMAP bitmap, ULONG64 chunk_index, ULONG64 new_value, ULONG64 expected_value) {
    if (bitmap == NULL) {
        return MAXULONG64;
    }

    ULONG64 size_in_chunks = (bitmap->size_in_bits + 63ULL) / 64ULL;
    if (chunk_index >= size_in_chunks) {
        return MAXULONG64;
    }

    ULONG64 actual_old_chunk_value = InterlockedCompareExchange64((volatile LONG64*)&bitmap->data[chunk_index],
        new_value, expected_value);

    // If actual_old_chunk_value == expected_value, then our swap succeeded.
    // Otherwise, another thread changed it, and we did not write new_value.
    if (actual_old_chunk_value == expected_value) {
        LONG64 old_set_bits_count = (LONG64)__popcnt64(actual_old_chunk_value);
        LONG64 new_set_bits_count = (LONG64)__popcnt64(new_value);

        LONG64 delta_set_bits = new_set_bits_count - old_set_bits_count;

        // Use InterlockedAdd64 to apply the delta
        InterlockedAdd64((volatile LONG64*)&bitmap->unset_spaces, -delta_set_bits);
    }

    return actual_old_chunk_value;
}

ULONG64 bitmap_get_unset_spaces(PINTERLOCKED_BITMAP bitmap) {
    if (bitmap == NULL) {
        return MAXULONG64;
    }
    return bitmap->unset_spaces;
}

ULONG64 bitmap_get_set_spaces(PINTERLOCKED_BITMAP bitmap) {
    if (bitmap == NULL) {
        return MAXULONG64;
    }
    return bitmap->size_in_bits - bitmap->unset_spaces;
}

ULONG64 bitmap_search_for_set_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 start_index, BOOLEAN unset_bit) {
    if (bitmap == NULL || start_index >= bitmap->size_in_bits) {
        return MAXULONG64;
    }

    ULONG64 current_chunk_index = bitmap_get_chunk_index(start_index);
    ULONG current_offset_in_chunk = bitmap_get_bit_offset_in_chunk(start_index);

    ULONG64 total_chunks = (bitmap->size_in_bits + 63ULL) / 64ULL;

    for (ULONG64 chunk_idx = current_chunk_index; chunk_idx < total_chunks; ++chunk_idx) {
        ULONG64 chunk_value = bitmap_get_chunk_value(bitmap, chunk_idx);

        if (chunk_value == 0) {
            // If the chunk is empty, continue to the next chunk
            current_offset_in_chunk = 0;
            continue;
        }

        for (ULONG offset = current_offset_in_chunk; offset < 64; ++offset) {
            // If the bit at this offset is set, we found a candidate
            if (chunk_value & (1ULL << offset)) {
                ULONG64 found_bit_index = (chunk_idx * 64ULL) + offset;
                if (unset_bit) {
                    // Attempt to atomically unset the bit. bitmap_unset_bit will update unset_spaces.
                    BOOLEAN unset_result = bitmap_unset_bit(bitmap, found_bit_index);
                    if (!unset_result) {
                        return found_bit_index;
                    }
                }
                return found_bit_index;
            }
        }
    }

    return MAXULONG64;
}


ULONG64 bitmap_search_for_unset_bit(PINTERLOCKED_BITMAP bitmap, ULONG64 start_index, BOOLEAN set_bit) {
    if (bitmap == NULL || start_index >= bitmap->size_in_bits) {
        return MAXULONG64;
    }

    ULONG64 current_chunk_index = bitmap_get_chunk_index(start_index);
    ULONG current_offset_in_chunk = bitmap_get_bit_offset_in_chunk(start_index);

    ULONG64 total_chunks = (bitmap->size_in_bits + 63ULL) / 64ULL;

    for (ULONG64 chunk_idx = current_chunk_index; chunk_idx < total_chunks; ++chunk_idx) {
        ULONG64 chunk_value = bitmap_get_chunk_value(bitmap, chunk_idx);

        if (chunk_value == ~0ULL) {
            // All bits set in this chunk, skip it
            current_offset_in_chunk = 0;
            continue;
        }

        for (ULONG offset = current_offset_in_chunk; offset < 64; ++offset) {
            ULONG64 bit_mask = (1ULL << offset);
            if ((chunk_value & bit_mask) == 0) {
                ULONG64 found_bit_index = (chunk_idx * 64ULL) + offset;
                if (found_bit_index >= bitmap->size_in_bits) {
                    break;
                }

                if (set_bit) {
                    // Try to set the bit atomically. bitmap_set_bit will update internal state.
                    BOOLEAN set_result = bitmap_set_bit(bitmap, found_bit_index);
                    if (!set_result) {
                        // Failed to set (e.g. already set by another thread), retry next bit
                        continue;
                    }
                }

                return found_bit_index;
            }
        }

        current_offset_in_chunk = 0;
    }

    return MAXULONG64;
}
