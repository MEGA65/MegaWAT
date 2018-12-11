#include "fileio.h"

uint8_t file_name[FILE_NAME_MAX_LEN] = "";
uint8_t drive = 8;

uint8_t last_message[32];

static FILE *file;
static uint8_t i, line = 0;
static uint32_t j, sz, diff;

// /*
static uint8_t c;
uint8_t get_error_code(uint8_t LFN)
{
    static int rtn = 0;
    cbm_k_clrch();
    c = '0';
    i = 0;
    do {
        rtn = (rtn * 10) + (c - '0');
        cbm_k_chkin(LFN);
        c = cbm_k_basin();
        last_message[i++] = c;
    } while (c != 0x2C);
    do {
        cbm_k_chkin(LFN);
        c = cbm_k_basin();
        last_message[i++] = c;
    } while (c != 0x0D);
    last_message[i] = 0;
    return rtn;
}

uint8_t command[42];
uint8_t drive_command(uint8_t CMD)
{
    static uint8_t i;
    for (i = 0; command[i] != 0; ++i)
    {
        cbm_k_ckout(CMD);
        cbm_k_bsout(command[i]);
    }
    return get_error_code(CMD);
}
// */

const static uint8_t mwb[] = ".mwb";
const static uint8_t mwt[] = ".mwt";
uint8_t backup_pres(uint8_t CMD)
{
    const static uint8_t renamef[] = "r:";
    const static uint8_t scratch[] = "s:";
    static uint8_t i, l;
    l = strlen(file_name);

    // Scratch previous backup
    i = 0;
    lcopy(scratch, command + i, sizeof(scratch) - 1);
    i += sizeof(scratch) - 1;
    lcopy(file_name, command + i, l);
    i += l;
    lcopy(mwb, command + i, sizeof(mwb) - 1);
    i += sizeof(mwb) - 1;
    command[i++] = '\n';
    command[i] = 0;
    drive_command(CMD);

    // Make a new backup
    i = 0;
    lcopy(renamef, command + i, sizeof(renamef) - 1);
    i += sizeof(renamef) - 1;
    lcopy(file_name, command + i, l);
    i += l;
    lcopy(mwb, command + i, sizeof(mwb) - 1);
    i += sizeof(mwb) - 1;
    command[i++] = '=';
    lcopy(file_name, command + i, l);
    i += l;
    lcopy(mwt, command + i, sizeof(mwt) - 1);
    i += sizeof(mwt) - 1;
    command[i++] = '\n';
    command[i] = 0;
    return drive_command(CMD);
}

// fread/fwrite are limited to 16 bits,
// so we need a buffer in the 16 pointer range
static uint8_t data_buffer[128];

int fileio_save_pres(void)
{
    static int rtn;
    static uint8_t i;
    cbm_k_setnam("");
    cbm_k_setlfs(15, drive, 15);
    cbm_k_open();
    backup_pres(15);
    cbm_k_close(15);

    rtn = 0;
    i = 0;
    for (; file_name[i] != 0; ++i)
        data_buffer[i] = file_name[i];
    lcopy(mwt, data_buffer + i, sizeof(mwt));
    file = fopen(data_buffer, "w");
    if (file)
    {
        for (i = 0; i < EDITOR_END_SLIDE; ++i)
        {
            sz = slide_start[i + 1] - slide_start[i];
            if (sizeof(sz) != fwrite(&sz, 1, sizeof(sz), file))
            {
                editor_show_message(line++, "failed to write size");
                for (;;) TOGGLE_BACK();
            }
            TOGGLE_BACK();

            if (sizeof(slide_colour[i]) != fwrite(&slide_colour[i], 1, sizeof(slide_colour[i]), file))
            {
                editor_show_message(line++, "failed to write slide_colour");
                for (;;) TOGGLE_BACK();
            }
            TOGGLE_BACK();

            for (j = 0; j < sz; j += sizeof(data_buffer))
            {
                diff = sz - j;
                if (diff > sizeof(data_buffer))
                    diff = sizeof(data_buffer);
                lcopy(slide_start[i] + j, data_buffer, diff);
                if (diff != fwrite(data_buffer, 1, diff, file))
                {
                    editor_show_message(line++, "failed to write slide_start");
                    for (;;) TOGGLE_BACK();
                }
                TOGGLE_BACK();
            }
            TOGGLE_BACK();
        }
        rtn = fclose(file);
    }
    else
    {
        rtn = errno;
        editor_show_message(line++, "failed to open file");
        editor_show_message(line++, strerror(rtn));
        editor_show_message(line++, "RETURN: ok");
        editor_show_message(line++, "");
        for (;READ_KEY() != KEY_RETURN;);
    }
    videoSetSlideMode();
    return rtn;
}

int fileio_load_pres(void)
{
    static int rtn;
    static uint8_t i;
    rtn = 0;
    line = 0;
    i = 0;
    for (; file_name[i] != 0; ++i)
        data_buffer[i] = file_name[i];
    lcopy(mwt, data_buffer + i, sizeof(mwt));
    file = fopen(data_buffer, "r");
    if (file)
    {
        slide_start[0] = SLIDE_DATA;
        for (i = 0; i < EDITOR_END_SLIDE; ++i)
        {
            if (sizeof(sz) != fread(&sz, 1, sizeof(sz), file))
            {
                editor_show_message(line++, "failed to read size");
                for (;;) TOGGLE_BACK();
            }
            TOGGLE_BACK();

            slide_start[i + 1] = slide_start[i] + sz;
            if (sizeof(slide_colour[i]) != fread(&slide_colour[i], 1, sizeof(slide_colour[i]), file))
            {
                editor_show_message(line++, "failed to read slide_colour");
                for (;;) TOGGLE_BACK();
            }
            TOGGLE_BACK();

            for (j = 0; j < sz; j += sizeof(data_buffer))
            {
                diff = sz - j;
                if (diff > sizeof(data_buffer))
                    diff = sizeof(data_buffer);
                if (diff != fread(data_buffer, 1, diff, file))
                {
                    editor_show_message(line++, "failed to write slide_start");
                    for (;;) TOGGLE_BACK();
                }
                lcopy(data_buffer, slide_start[i] + j, diff);
                TOGGLE_BACK();
            }
            TOGGLE_BACK();
        }
        rtn = fclose(file);
    }
    else
    {
        rtn = errno;
        editor_show_message(line++, "failed to open file");
        editor_show_message(line++, strerror(rtn));
        editor_show_message(line++, "RETURN: ok");
        editor_show_message(line++, "");
        for (;READ_KEY() != KEY_RETURN;);
    }
    videoSetSlideMode();
    return rtn;
}

const static uint8_t f65[] = ".f65";
int fileio_load_font(void)
{
    static int rtn;
    static uint8_t i;
    rtn = 0;
    line = 0;
    i = 0;
    for (; file_name[i] != 0; ++i)
        data_buffer[i] = file_name[i];
    lcopy(f65, data_buffer + i, sizeof(f65) - 1);
    file = fopen(data_buffer, "r");
    if (file)
    {
        current_font = next_font_address;
        sz = 0;
        for (i = sizeof(data_buffer); i == sizeof(data_buffer); sz += i)
        {
            i = fread(data_buffer, 1, sizeof(data_buffer), file);
            lcopy(data_buffer, current_font + sz, i);
        }
        next_font_address += sz;
        patchFont();
    } else rtn = errno;
    return rtn;
}