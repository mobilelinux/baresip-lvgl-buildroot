#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include "database_manager.h"

#include <stdarg.h>

// Stubs for dependencies
void logger_log(int level, const char *tag, const char *fmt, ...) {
    printf("[%s] ", tag);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}
void config_manager_init(void) {}
void config_get_dir_path(char *buf, size_t size) {
    // Current dir
    snprintf(buf, size, ".");
}

// Global DB
extern sqlite3 *g_db;

int main(int argc, char **argv) {
    printf("Test: Starting Contact Lookup Test\n");

    // 1. Init DB (creates tables)
    if (db_init() != 0) {
        printf("Test: db_init failed\n");
        return 1;
    }

    // 2. Insert Contact
    printf("Test: Inserting Contact Alice=1001\n");
    const char *sql = "INSERT INTO contacts (id, name, number) VALUES (1, 'Alice', '1001');";
    char *errmsg = NULL;
    int rc = sqlite3_exec(db_get_handle(), sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        printf("Test: Insert failed: %s\n", errmsg);
        // Ignore if exists (ID 1)
    }

    // 3. Lookup Contact
    char name[256];
    if (db_contact_find("1001", name, sizeof(name)) == 0) {
        printf("Test: Lookup '1001' Result: '%s'\n", name);
        if (strcmp(name, "Alice") == 0) {
            printf("Test: SUCCESS: Found Alice\n");
        } else {
            printf("Test: FAILURE: Name mismatch\n");
            return 1;
        }
    } else {
        printf("Test: FAILURE: Lookup '1001' not found\n");
        return 1;
    }

    // 4. Lookup Unknown
    if (db_contact_find("9999", name, sizeof(name)) == 0) {
        printf("Test: FAILURE: Found '9999' unexpectedly: '%s'\n", name);
        return 1;
    } else {
        printf("Test: SUCCESS: '9999' not found (correct)\n");
    }

    db_close();
    return 0;
}
