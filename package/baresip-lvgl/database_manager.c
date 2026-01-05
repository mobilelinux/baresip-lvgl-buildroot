#include "database_manager.h"
#include "config_manager.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *g_db = NULL;

int db_init(void) {
  if (g_db)
    return 0; // Already initialized

  char path[256];
  config_manager_init(); // Ensure config dir exists
  config_get_dir_path(path, sizeof(path));
  strcat(path, "/baresip.db");

  int rc = sqlite3_open(path, &g_db);
  if (rc) {
    printf("DatabaseManager: ERROR: Can't open database: %s\n", sqlite3_errmsg(g_db));
    // log_warn("DatabaseManager", "Can't open database: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  printf("DatabaseManager: SUCCESS: Opened database at %s\n", path);
  
  // Create Contacts table
  char *errmsg = NULL;
  rc = sqlite3_exec(g_db, "CREATE TABLE IF NOT EXISTS contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, number TEXT NOT NULL);", NULL, NULL, &errmsg);
  if (rc != SQLITE_OK) {
      log_error("DatabaseManager", "Failed to create contacts table: %s", errmsg);
      sqlite3_free(errmsg);
  }

  // Add is_favorite
  sqlite3_exec(g_db, "ALTER TABLE contacts ADD COLUMN is_favorite INTEGER DEFAULT 0;", NULL, NULL, NULL);

  // Create Call Log
  rc = sqlite3_exec(g_db, "CREATE TABLE IF NOT EXISTS call_log (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, number TEXT, type INTEGER, timestamp INTEGER, account_aor TEXT);", NULL, NULL, &errmsg);
  if (rc != SQLITE_OK) {
      log_error("DatabaseManager", "Failed to create call_log table: %s", errmsg);
      sqlite3_free(errmsg);
  }

  // Migration: Fix number
  sqlite3_exec(g_db, "UPDATE contacts SET number = 'sip:808086@fanvil.com' WHERE number = 'sip:8080866@fanvil.com';", NULL, NULL, NULL);

  return 0;
}

void db_close(void) {
  if (g_db) {
    sqlite3_close(g_db);
    g_db = NULL;
    log_info("DatabaseManager", "Closed database");
  }
}

sqlite3 *db_get_handle(void) { return g_db; }

int db_contact_find(const char *number, char *name_out, size_t size) {
  if (!g_db || !number || !name_out || size == 0) return -1;
  const char *sql = "SELECT name FROM contacts WHERE number = ?;";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;
  
  sqlite3_bind_text(stmt, 1, number, -1, SQLITE_STATIC);
  int result = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *name = (const char *)sqlite3_column_text(stmt, 0);
      if (name) {
          strncpy(name_out, name, size - 1);
          name_out[size - 1] = '\0';
          result = 0;
      }
  }
  sqlite3_finalize(stmt);
  return result;
}

int db_get_contacts(db_contact_t *contacts, int max_count) {
    if (!g_db || !contacts || max_count <= 0) return 0;
    
    const char *sql = "SELECT id, name, number FROM contacts LIMIT ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        log_error("DatabaseManager", "Failed to prepare get_contacts: %s", sqlite3_errmsg(g_db));
        return 0;
    }
    
    sqlite3_bind_int(stmt, 1, max_count);
    
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
        db_contact_t *c = &contacts[count];
        c->id = sqlite3_column_int(stmt, 0);
        
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if (name) strncpy(c->name, name, sizeof(c->name)-1);
        else c->name[0] = 0;
        
        const char *num = (const char *)sqlite3_column_text(stmt, 2);
        if (num) strncpy(c->number, num, sizeof(c->number)-1);
        else c->number[0] = 0;
        
        count++;
    }
    
    sqlite3_finalize(stmt);
    return count;
}

int db_get_favorite_contacts(db_contact_t *contacts, int max_count) {
    if (!g_db || !contacts || max_count <= 0) return 0;
    
    // Filter by is_favorite = 1
    const char *sql = "SELECT id, name, number FROM contacts WHERE is_favorite=1 LIMIT ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        log_error("DatabaseManager", "Failed to prepare get_favorites: %s", sqlite3_errmsg(g_db));
        return 0;
    }
    
    sqlite3_bind_int(stmt, 1, max_count);
    
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
        db_contact_t *c = &contacts[count];
        c->id = sqlite3_column_int(stmt, 0);
        
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if (name) strncpy(c->name, name, sizeof(c->name)-1);
        else c->name[0] = 0;
        
        const char *num = (const char *)sqlite3_column_text(stmt, 2);
        if (num) strncpy(c->number, num, sizeof(c->number)-1);
        else c->number[0] = 0;
        
        count++;
    }
    
    sqlite3_finalize(stmt);
    return count;
}
