#ifndef DATABASE_MANAGER_H
#define DATABASE_MANAGER_H

#include <sqlite3.h>
#include <stddef.h>

int db_init(void);
void db_close(void);
sqlite3 *db_get_handle(void);

// Lookup contact name by number/URI
// Returns 0 on success (found), -1 on not found/error
int db_contact_find(const char *number, char *name_out, size_t size);

typedef struct {
    int id;
    char name[64];
    char number[128];
} db_contact_t;

// Get all contacts using C API (limit to max_count)
int db_get_contacts(db_contact_t *contacts, int max_count);
int db_get_favorite_contacts(db_contact_t *contacts, int max_count);

#endif
