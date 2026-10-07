#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>

#include "config.h"

static void print_log(int line, const char* msg, const char* extra)
{
    printf("TConfig [Line: %i]: ", line);
    printf(msg, extra);
    printf("\n");
}

static ini_entry_s* _ini_entry_create(ini_section_s* section,
        const char* key, const char* value)
{
    if ((section->size % 10) == 0) {
        section->entry = 
            (ini_entry_s*)realloc(section->entry, (10+section->size) * sizeof(ini_entry_s));
    }
    ini_entry_s* entry = &section->entry[section->size++];
    entry->key = (char*)malloc((strlen(key)+1)*sizeof(char));
    entry->value = (char*)malloc((strlen(value)+1)*sizeof(char));
    strcpy(entry->key, key);
    strcpy(entry->value, value);
    return entry;
}

static ini_section_s* _ini_section_create(ini_table_s* table,
    const char* section_name)
{
    if ((table->size % 10) == 0) {
        table->section = 
            (ini_section_s*)realloc(table->section, (10+table->size) * sizeof(ini_section_s));
    }
    ini_section_s* section = &table->section[table->size++];
    section->size = 0;
    section->name = (char*)malloc((strlen(section_name)+1) * sizeof(char));
    strcpy(section->name, section_name);
    section->entry = (ini_entry_s*)malloc(10 * sizeof(ini_entry_s));
    return section;
}

static ini_section_s* _ini_section_find(ini_table_s* table, const char* name)
{
    for (int i = 0; i < table->size; i++) {
        if (strcmp(table->section[i].name, name) == 0) {
            return &table->section[i];
        }
    }
    return NULL;
}

static ini_entry_s* _ini_entry_find(ini_section_s* section, const char* key)
{
    for (int i = 0; i < section->size; i++) {
        if (strcmp(section->entry[i].key, key) == 0) {
            return &section->entry[i];
        }
    }
    return NULL;
}

static ini_entry_s* _ini_entry_get(ini_table_s* table, const char* section_name,
        const char* key)
{
    ini_section_s* section = _ini_section_find(table, section_name);
    if (section == NULL) {
        return NULL;
    }
    
    ini_entry_s* entry = _ini_entry_find(section, key);
    if (entry == NULL) {
        return NULL;
    }
    return entry;
}

ini_table_s* ini_table_create()
{
    ini_table_s* table = (ini_table_s*)malloc(sizeof(ini_table_s));
    table->size = 0;
    table->section = (ini_section_s*)malloc(10 * sizeof(ini_section_s));
    return table;
}

void ini_table_destroy(ini_table_s* table)
{
    for (int i = 0; i < table->size; i++) {
        ini_section_s* section = &table->section[i];
        for (int q = 0; q < section->size; q++) {
            ini_entry_s* entry = &section->entry[q];
            free(entry->key);
            free(entry->value);
        }
        free(section->entry);
        free(section->name);
    }
    free(table->section);
    free(table);
}

// Grows the line buffer by 128 bytes. The new tail is zeroed (strings in buf
// rely on it) and `value`, which points into buf, follows the new block.
static bool _ini_grow(char** buf, int* buffer_size, char** value)
{
    const ptrdiff_t valueOffset = *value ? *value - *buf : -1;
    char* grown = (char*)realloc(*buf, *buffer_size + 128);
    if (grown == NULL) return false;
    memset(grown + *buffer_size, '\0', 128);
    *buffer_size += 128;
    *buf = grown;
    if (valueOffset >= 0) *value = grown + valueOffset;
    return true;
}

// A key seen twice in one section (a tool appended to the file): the later
// line wins, as in other ini readers, instead of keeping a stale duplicate.
static void _ini_entry_store(ini_section_s* section, const char* key, const char* value)
{
    ini_entry_s* entry = key[0] != ';' ? _ini_entry_find(section, key) : NULL;
    if (entry == NULL) {
        _ini_entry_create(section, key, value);
        return;
    }
    char* copy = (char*)malloc((strlen(value)+1)*sizeof(char));
    if (copy == NULL) return;
    strcpy(copy, value);
    free(entry->value);
    entry->value = copy;
}

bool ini_table_read_from_file(ini_table_s* table, const char* file)
{
    FILE* f = fopen(file, "r");
    if (f == NULL) return false;

    enum {Section, Key, Value, Comment} state = Section;
    int   c;
    int   position = 0;
    int   spaces   = 0;
    int   line     = 0;
    int   buffer_size = 128 * sizeof(char);
    char* buf   = (char*)malloc(buffer_size);
    char* value = NULL;
    bool  ok    = buf != NULL;

    ini_section_s* current_section = NULL;

    if (ok) memset(buf, '\0', buffer_size);
    while(ok && (c = getc(f)) != EOF) {
        while (ok && position + spaces > buffer_size-2) ok = _ini_grow(&buf, &buffer_size, &value);
        if (!ok) break;
        switch(c) {
            case ' ':
                switch(state) {
                    case Value: if (value[0] != '\0') spaces++; break;
                    default: if (buf[0] != '\0') spaces++; break;
                }
                break;
            case ';':
                if (state == Value) {
                    buf[position++] = c;
                    break;
                } else {
                    state = Comment;
                    buf[position++] = c;
                    while (c != EOF && c != '\n') {
                       c = getc(f);
                       if (c == EOF || c == '\n') break;
                       if (position > buffer_size-2 && !_ini_grow(&buf, &buffer_size, &value)) { ok = false; break; }
                       buf[position++] = c;
                    }
                    if (!ok) break;
                }
            // fallthrough
            case '\n':
                line++;
                if (state == Value) {
                    if (current_section == NULL) {
                        current_section = _ini_section_create(table, "");
                    }
                    _ini_entry_store(current_section, buf, value);
                } else if (state == Comment) {
                    if (current_section == NULL) {
                        current_section = _ini_section_create(table, "");
                    }
                    _ini_entry_store(current_section, buf, "");
                } else if (state == Section) {
                    print_log(line, "Section `%s' missing `]' operator.", buf);
                } else if(state == Key && position) {
                    print_log(line, "Key `%s' missing `=' operator.", buf);
                }
                memset(buf, '\0', buffer_size);
                state = Key;
                position = 0;
                spaces = 0;
                value = NULL;
                break;
            case '[':
            case ']':
                // Inside a value ("name = [TAG]Nick") brackets are plain text.
                if (state == Value) {
                    for(;spaces > 0; spaces--) buf[position++] = ' ';
                    buf[position++] = c;
                    break;
                }
                if (c == '[') {
                    state = Section;
                    break;
                }
                current_section = _ini_section_create(table, buf);
                memset(buf, '\0', buffer_size);
                position = 0;
                spaces = 0;
                state = Key;
                break;
            case '=':
                if (state == Key) {
                    state = Value;
                    buf[position++] = '\0';
                    value = buf + position;
                    spaces = 0;
                    continue;
                }
            // fallthrough
            default:
                for(;spaces > 0; spaces--) buf[position++] = ' ';
                buf[position++] = c;
                break;
        }
    }
    // Last line without a final newline (files written by other tools).
    if (ok && state == Value) {
        if (current_section == NULL) {
            current_section = _ini_section_create(table, "");
        }
        _ini_entry_store(current_section, buf, value);
    }
    free(buf);
    fclose(f);
    return ok;
}

bool ini_table_write_to_file(ini_table_s* table, const char* file)
{
    FILE* f = fopen(file, "w+");
    if (f == NULL) return false;
    for (int i = 0; i < table->size; i++) {
        ini_section_s* section = &table->section[i];
        fprintf(f, i > 0 ? "\n[%s]\n" : "[%s]\n", section->name);
        for (int q = 0; q < section->size; q++) {
            ini_entry_s* entry = &section->entry[q];
            if (entry->key[0] == ';') {
                fprintf(f, "%s\n", entry->key);
            } else {
                fprintf(f, "%s = %s\n", entry->key, entry->value);
            }
        }
    }
    // Report a full disk / failed write instead of "saved".
    bool ok = ferror(f) == 0;
    if (fflush(f) != 0) ok = false;
    if (ok) fsync(fileno(f));
    if (fclose(f) != 0) ok = false;
    return ok;
}


void ini_table_create_entry(ini_table_s* table, const char* section_name,
        const char* key, const char* value)
{
    ini_section_s* section = _ini_section_find(table, section_name);
    if (section == NULL) {
        section = _ini_section_create(table, section_name);
    }
    ini_entry_s* entry = _ini_entry_find(section, key);
    if (entry == NULL) {
        entry = _ini_entry_create(section, key, value);
    }else {
        free(entry->value);
        entry->value = (char*)malloc((strlen(value)+1) * sizeof(char));
        strcpy(entry->value, value);
    }
}

void ini_table_create_entry_as_int(ini_table_s* table, const char* section_name,
	const char* key, int value)
{
	char buff[128];
	snprintf(buff, sizeof(buff), "%d", value);
	ini_table_create_entry(table, section_name, key, buff);
}

void ini_table_create_entry_as_float(ini_table_s* table, const char* section_name,
	const char* key, float value)
{
	char buff[128];
	snprintf(buff, sizeof(buff), "%f", value);
	ini_table_create_entry(table, section_name, key, buff);
}

bool ini_table_check_entry(ini_table_s* table, const char* section_name,
        const char* key)
{
    return (_ini_entry_get(table, section_name, key) != NULL);
}

const char* ini_table_get_entry(ini_table_s* table, const char* section_name,
        const char* key)
{
    ini_entry_s* entry = _ini_entry_get(table, section_name, key);
    if (entry == NULL) {
        return NULL;
    }
    return entry->value;
}

int ini_table_get_entry_as_int(ini_table_s* table, const char* section_name,
        const char* key, int defaultValue)
{
    const char* val = ini_table_get_entry(table, section_name, key);
    if (val == NULL) 
	{
		return defaultValue;
    }
	return atoi(val);
}

void Log(const char* fmt, ...);

float ini_table_get_entry_as_float(ini_table_s* table, const char* section_name,
	const char* key, float defaultValue)
{
	const char* val = ini_table_get_entry(table, section_name, key);
	if (val == NULL) 
	{
		return defaultValue;
	}
	float retn = (float)atof(val);
	if (retn == 0.0f)
	{
		retn = (float)atoi(val);
	}
	return retn;
}

bool ini_table_get_entry_as_bool(ini_table_s* table, const char* section_name,
        const char* key, bool* value)
{
    const char* val = ini_table_get_entry(table, section_name, key);
    if (val == NULL) {
        return false;
    }
    if (strcasecmp(val, "on") == 0 || strcasecmp(val, "true") == 0) {
        *value = true;
    }else {
        *value = false;
    }
    return true;
}

