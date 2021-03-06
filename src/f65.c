#include "f65.h"
#include "memory.h"

/*
  NOTE: Fonts are stored outside of the 64KB address space.
  Therefore you cannot use pointer types to point to them, because
  CC65 doesn't understand them, and will use 16-bit pointers.
  In fact, CC65 will do a HORRIBLE job at any access to memory >64KB,
  in part because it doesn't know about the 32-bit indirect addressing
  mode of the MEGA65's CPU.
*/

// TODO: replace 59 with screen_height-1

// List of fonts
uint32_t font_addresses[MAX_FONTS];
uint8_t font_count = 0;

// Current font
uint8_t font_id = 0;      // ID of the current font
ptr_t current_font = 0U;  // address of the current font

uint8_t start_column;
uint16_t glyph_count = 0U;

// point list
ptr_t point_list_address = 0U, point_list_size = 0U;

// tile map
uint16_t tile_map_start = 0U;
ptr_t tile_map_address = 0U;

// tile array
longptr_t tile_array_start = 0U, tile_array_address = 0U;

// Size of font
uint32_t font_size = 0U;

ptr_t screen = 0U, colour = 0U;
uint8_t bytes_per_row;
uint8_t rows_above;
uint8_t rows_below;
ptr_t map_pos;
// ptr_t glyph_height, glyph_width, glyph_size;
ptr_t glyph_size;
uint8_t trim_pixels;
uint16_t the_code_point = 0U;

uint8_t clear_pattern[4] = {0x20, 0x00, 0x20, 0x00};

render_buffer_t screen_rbuffer;
render_buffer_t scratch_rbuffer;
render_buffer_t *active_rbuffer = 0;

// void renderTextUTF16(uint16_t *str, uint8_t colour_and_attributes, uint8_t alpha_and_extras)
// {
//     for (x = 0; str[x]; ++x)
//         renderGlyph(str[x],
//                     colour_and_attributes, // Various colours
//                     alpha_and_extras,      // alpha blend
//                     0xFF                   // Append to end
//         );
// }

// void renderTextASCII(uint8_t *str, uint8_t colour_and_attributes, uint8_t alpha_and_extras)
// {
//     for (x = 0; str[x]; ++x)
//         renderGlyph(str[x],
//                     colour_and_attributes, // Various colours
//                     alpha_and_extras,      // alpha blend
//                     0xFF                   // Append to end
//         );
// }

void clearRenderBuffer(void)
{
    if (!active_rbuffer)
        return;

    // Fill colour RAM with 0x00, so that it is blank.
    lfill(active_rbuffer->colour_ram, 0, active_rbuffer->screen_size);

    // Fill screen RAM with 0x20 0x00 pattern, so that it is blank.
    lcopy((ptr_t)&clear_pattern[0], active_rbuffer->screen_ram, sizeof(clear_pattern));
    // Fill out to whole line
    lcopy(active_rbuffer->screen_ram, active_rbuffer->screen_ram + sizeof(clear_pattern), (screen_width - sizeof(clear_pattern)));
    // Then copy it down over the next 59 rows.
    lcopy(active_rbuffer->screen_ram, active_rbuffer->screen_ram + screen_width, active_rbuffer->screen_size - screen_width);

    active_rbuffer->columns_used = 0;
    active_rbuffer->max_above = 0;
    active_rbuffer->max_below = 0;
    active_rbuffer->baseline_row = 24;
    active_rbuffer->trimmed_pixels = 0;
    active_rbuffer->glyph_count = 0;
}

void outputLineToRenderBuffer(void)
{
    static uint32_t i, j;
    if ((scratch_rbuffer.max_above + scratch_rbuffer.max_below) < 1)
        return;

    // Get total number of rows we need to actually output
    rows_below = scratch_rbuffer.max_above + scratch_rbuffer.max_below;
    if ((rows_below + screen_rbuffer.rows_used) > 59)
        rows_below = 59 - screen_rbuffer.rows_used;

    // And work out which is the first one
    rows_above = scratch_rbuffer.baseline_row - scratch_rbuffer.max_above;

    i = rows_above * screen_width;
    j = rows_below * screen_width;

    // Do the copies via DMA
    lcopy(scratch_rbuffer.screen_ram + i, screen_rbuffer.screen_ram + (screen_rbuffer.rows_used * screen_width), j);
    lcopy(scratch_rbuffer.colour_ram + i, screen_rbuffer.colour_ram + (screen_rbuffer.rows_used * screen_width), j);

    // Mark the rows used in the output buffer
    screen_rbuffer.rows_used += rows_below;
}

void setFont(uint8_t id)
{
    font_id = id < font_count + 1 ? id : 0;
    if (font_id > 0)
    {
        current_font = font_addresses[font_id - 1];
        findFontStructures();
    } else current_font = 0;
}

void findFontStructures(void)
{
    lcopy(current_font + 0x80, (ptr_t)&glyph_count, 2);

    lcopy(current_font + 0x82, (ptr_t)&tile_map_start, 2);
    point_list_size = tile_map_start - 0x100;
    point_list_address = current_font + 0x100;
    tile_array_start = 0;
    lcopy(current_font + 0x84, (ptr_t)&tile_array_start, 3);
    tile_map_address = current_font + tile_map_start;
    tile_array_address = current_font + tile_array_start;

    lcopy(current_font + 0x8c, (ptr_t)&font_size, 4);
}

void deleteGlyph(uint8_t glyph_num)
{
    static uint8_t x, y;
    static uint32_t m;

    // Make sure buffer and glyph number are ok
    if (!active_rbuffer)
        return;
    if (glyph_num >= active_rbuffer->glyph_count)
        return;

    rows_above = active_rbuffer->max_above;
    rows_below = active_rbuffer->max_below;

    // Get start address for the first row we have to copy.
    m = ((active_rbuffer->baseline_row - rows_above) * screen_width) +
        (active_rbuffer->glyphs[glyph_num].first_column * char_size);
    screen = active_rbuffer->screen_ram + m;
    colour = active_rbuffer->colour_ram + m;

    x = active_rbuffer->glyphs[glyph_num].size.columns * char_size;
    bytes_per_row = (active_rbuffer->columns_used -
        (active_rbuffer->glyphs[glyph_num].first_column + active_rbuffer->glyphs[glyph_num].size.columns)) * char_size;

    // Copy remaining glyphs on the line
    if ((glyph_num + 1) < active_rbuffer->glyph_count)
    {
        for (y = 0; y < (rows_above + rows_below); ++y)
        {
            // Copy screen data
            lcopy_safe(screen + x, screen, bytes_per_row);
            // Copy colour data
            lcopy_safe(colour + x, colour, bytes_per_row);

            screen += screen_width;
            colour += screen_width;
        }
    }

    // Note if we need to recalculate rows above and rows below
    x = (active_rbuffer->glyphs[glyph_num].size.rows_above == rows_above) ||
        (active_rbuffer->glyphs[glyph_num].size.rows_below == rows_below)
            ? 1
            : 0;

    // Subtract the used columns
    active_rbuffer->columns_used -= active_rbuffer->glyphs[glyph_num].size.columns;

    // Now erase the tail of each line
    m = ((active_rbuffer->baseline_row - rows_above) * screen_width) +
        (active_rbuffer->columns_used * char_size);
    screen = active_rbuffer->screen_ram + m;
    colour = active_rbuffer->colour_ram + m;

    bytes_per_row = active_rbuffer->glyphs[glyph_num].size.columns * char_size;

    for (y = 0; y < (rows_above + rows_below); ++y)
    {
        // Clear screen data
        // lfill(screen, 0x20, bytes_per_row);

        // Fill screen RAM with 0x20 0x00 pattern, so that it is blank.
        lcopy((ptr_t)&clear_pattern[0], screen, sizeof(clear_pattern) < bytes_per_row ? sizeof(clear_pattern) : bytes_per_row);
        // Fill out to whole line
        if (bytes_per_row > sizeof(clear_pattern))
            lcopy(screen, screen + sizeof(clear_pattern), (bytes_per_row - sizeof(clear_pattern)));

        // Clear colour data
        lfill(colour, 0x00, bytes_per_row);

        screen += screen_width;
        colour += screen_width;
    }

    // Update the first_column field of each of the shoved glyphs
    for (y = glyph_num; y < active_rbuffer->glyph_count; ++y)
        active_rbuffer->glyphs[y].first_column -= active_rbuffer->glyphs[glyph_num].size.columns;

    // Now copy down the glyph details structure.
    lcopy_safe((ptr_t)&active_rbuffer->glyphs[glyph_num + 1], (ptr_t)&active_rbuffer->glyphs[glyph_num],
        sizeof(glyph_details_t) * (99 - glyph_num));

    // Reduce number of remaining glyphs
    --(active_rbuffer->glyph_count);

    // Update rows_above and rows_below if require
    if (x)
    {
        rows_above = 0;
        rows_below = 0;
        for (y = 0; y < active_rbuffer->glyph_count; ++y)
        {
            if (active_rbuffer->glyphs[y].size.rows_above > rows_above)
                rows_above = active_rbuffer->glyphs[y].size.rows_above;
            if (active_rbuffer->glyphs[y].size.rows_below > rows_below)
                rows_below = active_rbuffer->glyphs[y].size.rows_below;
        }
        active_rbuffer->max_above = rows_above;
        active_rbuffer->max_below = rows_below;
    }
}

void replaceGlyph(uint8_t glyph_num, uint16_t code_point);
void insertGlyph(uint8_t glyph_num, uint16_t code_point);

glyph_details_t *active_glyph = 0;
glyph_details_t glyph_buffer;
void getGlyphDetails(uint16_t code_point, uint8_t colour_and_attributes, uint8_t first_column)
{
    static uint32_t i;

    if (!active_glyph)
        return;

    active_glyph->code_point = code_point;
    active_glyph->font_id = font_id;
    active_glyph->first_column = first_column;
    active_glyph->colour_and_attributes = colour_and_attributes;
    // We have the glyph, so dig out the information on it.
    if (font_id)
    {
        for (i = 0; i < point_list_size; i += 5)
        {
            lcopy(point_list_address + i, (ptr_t)&the_code_point, 2);
            if (the_code_point == code_point)
            {
                // pointer copy is only 3 bytes, so make sure 4th is 0
                active_glyph->map_pos = 0;
                // deref point_list_address + i + 2 (glyph_size_t**) into active_glyph->map_pos (glyph_size_t*)
                lcopy(point_list_address + i + 2, (ptr_t)&(active_glyph->map_pos), 3);
                // deref active_gmap (glyph_size_t*) into active_glyph->size
                lcopy(active_glyph->map_pos, (ptr_t)&(active_glyph->size), sizeof(glyph_size_t));
                break;
            }
        }
    }
    else if (code_point <= 0xFF)
    {
        // Font ID 0 = C64 character ROM font
        // Valid only for 0x00 - 0xFF

        // C64 character ROM, so always 1x1 bytes, no trim.
        active_glyph->size.rows_above = 1;
        active_glyph->size.rows_below = 0;
        active_glyph->size.columns = 1;
        active_glyph->size.trim_pixels = 0;
    }
}

char renderGlyphDetails(uint8_t alpha_and_extras, uint8_t position)
{
    static uint8_t x, y, v1, v2, v3, v4, v5;
    static uint32_t i, l;

    if (!active_rbuffer)
        return 0;

    // Default to allowing 24 rows above and 6 rows below for underhangs
    if (!active_rbuffer->baseline_row)
        active_rbuffer->baseline_row = 24;

    if (position == 0xFF)
        position = active_rbuffer->glyph_count;

    // Make space if this glyph is not at the end
    start_column = (position < active_rbuffer->glyph_count)
        ? active_rbuffer->glyphs[position].first_column
        : active_rbuffer->columns_used;

    if (start_column > 99)
        return 0;

    bytes_per_row = active_glyph->size.columns;
    // If glyph is 0 pixels wide, nothing to do
    if (!bytes_per_row)
        return 1;
    // If glyph would overrun the buffer, don't draw it
    if ((bytes_per_row + active_rbuffer->columns_used) > 99)
        return 0;

    rows_above = active_glyph->size.rows_above;
    rows_below = active_glyph->size.rows_below;
    // Don't allow glyphs that go too far above or below baseline
    if (rows_above >= active_rbuffer->baseline_row ||
        rows_below >= (30 - active_rbuffer->baseline_row))
        return 0;

    // Ok, we have a glyph, so now we make space in the glyph list
    // This is an overlapping copy, so we have to copy to a temp location first
    if (position < active_rbuffer->glyph_count)
    {
        // Update the first_column field of each of the shoved glyphs
        for (y = position; y < active_rbuffer->glyph_count; ++y)
            active_rbuffer->glyphs[y].first_column += bytes_per_row;
        lcopy_safe((ptr_t)&active_rbuffer->glyphs[position], (ptr_t)&active_rbuffer->glyphs[position + 1],
            sizeof(glyph_details_t) * (active_rbuffer->glyph_count - position));
    }

    active_glyph->first_column = start_column;
    lcopy(active_glyph, (ptr_t)&(active_rbuffer->glyphs[position]), sizeof(glyph_details_t));

    bytes_per_row = bytes_per_row << 1;

    if (position < active_rbuffer->glyph_count)
    {
        // Char is not on the end, so make space

        // First, work out the address of the start row and column
        l = (start_column * char_size) + ((active_rbuffer->baseline_row - active_rbuffer->max_above) * screen_width);
        screen = active_rbuffer->screen_ram + l;
        colour = active_rbuffer->colour_ram + l;

        // Now we need to copy each row of data to the screen RAM and colour RAM buffers
        for (y = 0; y < (active_rbuffer->max_above + active_rbuffer->max_below); ++y)
        {
            // Make space
            lcopy_safe(screen, screen + bytes_per_row, (active_rbuffer->columns_used - start_column) * 2);
            lcopy_safe(colour, colour + bytes_per_row, (active_rbuffer->columns_used - start_column) * 2);

            // Fill screen and colour RAM with empty patterns
            lcopy((ptr_t)&clear_pattern, screen, bytes_per_row);
            lcopy(screen, screen + 2, bytes_per_row - 2);
            lfill(colour, 0x00, bytes_per_row);

            screen += screen_width;
            colour += screen_width;
        }
    }

    // Work out first address of screen and colour RAM

    l = (start_column * char_size) + ((active_rbuffer->baseline_row - rows_above) * screen_width);
    // First, work out the address of the start row and column
    screen = active_rbuffer->screen_ram + l;
    colour = active_rbuffer->colour_ram + l;

    // Now we need to copy each row of data to the screen RAM and colour RAM buffers
    if (font_id)
    {
        // Skip header (glyph_size_t)
        map_pos = active_glyph->map_pos + 4; // sizeof(glyph_size_t);
        for (y = 0; y < (rows_above + rows_below); ++y)
        {
            // Copy screen data
            lcopy(map_pos, screen, bytes_per_row);
            map_pos += bytes_per_row;
            screen += screen_width;
            colour += screen_width;
        }
    }
    else
    {
        // C64 character ROM, so just write code point in low byte, and empty high byte
        lpoke(screen, active_glyph->code_point);
        lpoke(screen + 1, 0x00);
    }

    active_rbuffer->columns_used += (bytes_per_row >> 1);
    v1 = active_rbuffer->glyphs[position].first_column << 1;
    v3 = active_rbuffer->columns_used << 1;
    v2 = (position < active_rbuffer->glyph_count)
        ? active_rbuffer->glyphs[position + 1].first_column << 1
        : v3;

    if (rows_above > active_rbuffer->max_above)
    {
        // Buffer became taller, we need to copy colour and trim details to new rows
        l = (active_rbuffer->baseline_row - active_rbuffer->max_above) * screen_width;
        colour = active_rbuffer->colour_ram + l;
        screen = active_rbuffer->screen_ram + l;
        for (; active_rbuffer->max_above < rows_above; ++(active_rbuffer->max_above))
        {
            for (y = 0; y < v3; y += 2)
            {
                lpoke((colour - screen_width) + y, lpeek(colour + y));
                v4 = lpeek(colour + y + 1);
                lpoke((colour - screen_width) + y + 1, v4 & ATTRIB_REVERSE
                        ? v4 & ~ATTRIB_UNDERLINE
                        : v4 & ATTRIB_UNDERLINE
                            ? (v4 & ~(ATTRIB_BLINK | ATTRIB_UNDERLINE)) : v4);
                // Only mess with screen data once we're out of character range
                if (y < v1 || y >= v2)
                {
                    lpoke((screen - screen_width) + y, 0x20);
                    lpoke((screen - screen_width) + y + 1, 0xE0 & lpeek(screen + y + 1));
                }
            }
            screen -= screen_width;
            colour -= screen_width;
        }
    }
    else
    {
        // Buffer is taller than the inserted glyph, make sure the top still has kerning info
        l = (active_rbuffer->baseline_row - rows_above) * screen_width;
        colour = active_rbuffer->colour_ram + l;
        screen = active_rbuffer->screen_ram + l;
        // Only mess with screen data once we're out of character range
        for (v5 = rows_above; v5 < active_rbuffer->max_above; ++v5)
        {
            for (y = v1; y < v2; y += 2)
            {
                lpoke((screen - screen_width) + y, 0x20);
                lpoke((screen - screen_width) + y + 1, 0xE0 & lpeek(screen + y + 1));
            }
            screen -= screen_width;
            colour -= screen_width;
        }
    }

    if (rows_below > active_rbuffer->max_below)
    {
        // Buffer became taller, we need to copy colour and trim details to new rows
        l = ((active_rbuffer->baseline_row + active_rbuffer->max_below) - 1) * screen_width;
        colour = active_rbuffer->colour_ram + l;
        screen = active_rbuffer->screen_ram + l;
        for (; active_rbuffer->max_below < rows_below; ++(active_rbuffer->max_below))
        {
            for (y = 0; y < v3; y += 2)
            {
                lpoke(colour + screen_width + y, lpeek(colour + y));
                v4 = lpeek(colour + y + 1);
                lpoke(colour + screen_width + y + 1, v4 & ATTRIB_REVERSE
                    ? v4 & ~ATTRIB_UNDERLINE
                    : v4 & ATTRIB_UNDERLINE
                        ? (v4 & ~(ATTRIB_BLINK | ATTRIB_UNDERLINE)) : v4);

                // Only mess with screen data once we're out of character range
                if (y < v1 || y >= v2)
                {
                    lpoke(screen + screen_width + y, 0x20);
                    lpoke(screen + screen_width + y + 1, 0xE0 & lpeek(screen + y + 1));
                }
            }
            screen += screen_width;
            colour += screen_width;
        }
    }
    else
    {
        // Buffer hangs lower than the character, make sure kerning information goes all the way down
        l = ((active_rbuffer->baseline_row + rows_below) - 1) * screen_width;
        colour = active_rbuffer->colour_ram + l;
        screen = active_rbuffer->screen_ram + l;
        // Only mess with screen data once we're out of character range
        for (v5 = rows_below; v5 < active_rbuffer->max_below; ++v5)
        {
            for (y = v1; y < v2; y += 2)
            {
                lpoke(screen + screen_width + y, 0x20);
                lpoke(screen + screen_width + y + 1, 0xE0 & lpeek(screen + y + 1));
            }
            screen += screen_width;
            colour += screen_width;
        }
    }

    // Copy the colour data to the full height of the buffer
    l = (start_column * char_size) + ((active_rbuffer->baseline_row - active_rbuffer->max_above) * screen_width);
    screen = active_rbuffer->screen_ram + l;
    colour = active_rbuffer->colour_ram + l;
    for (y = 0; y < (active_rbuffer->max_above + active_rbuffer->max_below); ++y)
    {
        // Fill colour RAM.
        // Bit 5 of low byte is alpha blending flag. This is true for font glyphs, and false
        // for graphics tiles.
        // Then bottom 4 bits of high byte is foreground colour, and the upper 4 bits are VIC-III
        // attributes (blink, underline etc)
        // This requires 0x20 for the first byte, to enable alpha blending
        v4 = active_glyph->colour_and_attributes;
        lpoke(colour, alpha_and_extras);
        if (y == active_rbuffer->max_above - 1)
            lpoke(colour + 1, v4);
        else
            lpoke(colour + 1, v4 & ATTRIB_REVERSE
                ? v4 & ~ATTRIB_UNDERLINE
                : v4 & ATTRIB_UNDERLINE
                    ? v4 & ~(ATTRIB_BLINK | ATTRIB_UNDERLINE)
                    : v4);
        if (bytes_per_row > 2)
        {
            lpoke(colour + 2, alpha_and_extras);
            if (y == active_rbuffer->max_above - 1)
                lpoke(colour + 3, v4);
            else
            lpoke(colour + 3, v4 & ATTRIB_REVERSE
                ? v4 & ~ATTRIB_UNDERLINE
                : v4 & ATTRIB_UNDERLINE
                    ? v4 & ~(ATTRIB_BLINK | ATTRIB_UNDERLINE)
                    : v4);
        }
        // Then use DMA to copy the pair of bytes out
        // (DMA timing is a bit funny, hence we need to have the two copies setup above
        //  for this to work reliably).
        if (bytes_per_row > 4)
            lcopy((ptr_t)colour, (ptr_t)colour + 2, bytes_per_row - 2);

        screen += screen_width;
        colour += screen_width;
    }

    active_rbuffer->glyph_count++;

    // then apply trim to entire column
    trim_pixels = active_glyph->size.trim_pixels << 5;
    screen = active_rbuffer->screen_ram + start_column + start_column + bytes_per_row - 1;
    for (y = 0; y < 30; ++y)
    {
        lpoke(screen, (lpeek(screen) & 0x1f) | trim_pixels);
        screen += screen_width;
    }
    return 1;
}

ptr_t next_font_address;
void patchFonts(void)
{
    static ptr_t font_address;

    if (!current_font) return;
    font_address = current_font; // backup the current font
    for (font_count = 0; font_count < MAX_FONTS && current_font < (ASSET_RAM + ASSET_RAM_SIZE);)
    {
        if (lpeek(current_font) == 0x3D)
            // already patched
            lcopy(current_font + 0x8c, (ptr_t)&font_size, 4);
        else
            patchFont();
        font_addresses[font_count++] = current_font;
        current_font += font_size;
        next_font_address = current_font;

        // Stop if end of list
        if (!font_size)
            break;
    }
    current_font = font_address; // reapply the active font
}

void patchFont(void)
{
    static uint16_t card_address;
    static uint32_t i, j;
    static ptr_t point_tile, tile_address, tile_cards;

    findFontStructures();
    if (!font_size)
        return;

    // Patch tile_array_start
    tile_array_start += (longptr_t)current_font;
    lcopy((ptr_t)&tile_array_start, current_font + 0x84, 3);

    tile_array_start /= 0x40;

    for (i = 0; i < point_list_size; i += 5)
    {
        // map_pos = 0;
        // lcopy(point_list + i + 2, (ptr_t)&map_pos, 3);
        // tile = map + map_pos;
        tile_address = 0;
        point_tile = point_list_address + i + 2;
        lcopy(point_tile, (ptr_t)&tile_address, 3);
        tile_address += (longptr_t)tile_map_address;
        lcopy((ptr_t)&tile_address, point_tile, 3);

        rows_above = lpeek(tile_address);
        rows_below = lpeek(tile_address + 1);
        bytes_per_row = lpeek(tile_address + 2);
        // tile_address + 3 -> trim_pixels
        tile_cards = tile_address + 4;
        glyph_size = (int16_t)(rows_above + rows_below) * bytes_per_row;
        for (j = 0; j < glyph_size; ++j)
        {
            lcopy(tile_cards, (ptr_t)&card_address, 2);
            // lcopy(gradient, (((longptr_t)card_address&0x0FFF) * 64) + tile_array_address, 64);
            card_address += (longptr_t)tile_array_start;
            lcopy((ptr_t)&card_address, tile_cards, 2);
            tile_cards += 2;
        }
    }

    lpoke(current_font, 0x3D);
}
