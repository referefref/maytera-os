// sqlitchk - the RUNNING proof for the mports SQLite port (userland/ports/sqlite)
// and its MayteraOS VFS (userland/ports/sqlite/maytera_vfs.c).
//
// WHY THIS APP EXISTS. "A library that compiles but was never linked and run is
// not evidence." This is the evidence. It links the static libsqlite3.a that
// userland/ports/mports.sh produced from the sha256-pinned upstream amalgamation
// plus our VFS, and it exercises a real round-trip on a booted machine: create
// a table, insert rows in a transaction, read them back, close, then REOPEN the
// on-disk file and confirm the rows persisted. That last step is the whole
// point of a custom VFS: it proves the database really reached a file on ext2
// through open/read/write/lseek/fstat, not an in-memory fiction.
//
// OUTPUT DISCIPLINE. Intended to run via /CONFIG/AUTORUN.CFG. An autorun-
// launched process emits ONE SERIAL RECORD PER write(), so every line here is
// formatted into a buffer and issued as exactly one write(2, ...) (blame.md,
// #700).

#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "sys/stat.h"

#include "sqlite3.h"

static int g_pass = 0, g_fail = 0;

static void line(const char *s) { write(2, s, strlen(s)); }

static void check(const char *label, int cond) {
  char b[256];
  snprintf(b, sizeof b, "%s%s\n", cond ? "[PASS] " : "[FAIL] ", label);
  if (cond) g_pass++; else g_fail++;
  line(b);
}

int main(void) {
  char buf[512];
  const char *path = "/WINDIR/DRIVE_C/SQLITE.DB";
  sqlite3 *db = 0;
  sqlite3_stmt *st = 0;
  sqlite3_vfs *vfs;
  char *err = 0;
  struct stat sstat;
  int rc, rows, sumval, okrow, cnt;

  line("=== sqlitchk: mports SQLite + MayteraOS VFS ===\n");

  // 1. Library version and, crucially, the default VFS. SQLITE_OS_OTHER=1 means
  //    the ONLY VFS that can be the default is the one sqlite3_os_init()
  //    registered, i.e. ours.
  vfs = sqlite3_vfs_find(0);
  snprintf(buf, sizeof buf, "[INFO] sqlite %s, default VFS = %s\n",
           sqlite3_libversion(), (vfs && vfs->zName) ? vfs->zName : "(none)");
  line(buf);
  check("default VFS is 'maytera'", vfs && vfs->zName && strcmp(vfs->zName, "maytera") == 0);

  // Start from a clean slate. unlink() is what our VFS xDelete calls.
  unlink(path);

  // 2. Open (this drives xOpen/xFullPathname/xAccess on the real file).
  rc = sqlite3_open(path, &db);
  check("sqlite3_open round-trips through the VFS", rc == SQLITE_OK && db != 0);
  if (rc != SQLITE_OK) {
    snprintf(buf, sizeof buf, "[FAIL] open error: %s\n", db ? sqlite3_errmsg(db) : "(no db)");
    line(buf);
  }

  // 3. Create a table.
  rc = sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, val INTEGER);",
                    0, 0, &err);
  check("CREATE TABLE", rc == SQLITE_OK);
  if (err) { snprintf(buf, sizeof buf, "[FAIL] %s\n", err); line(buf); sqlite3_free(err); err = 0; }

  // 4. Insert three rows inside a transaction (exercises the rollback journal,
  //    hence xWrite/xSync/xTruncate/xDelete on <db>-journal).
  rc = sqlite3_exec(db,
        "BEGIN;"
        "INSERT INTO t(name,val) VALUES('alpha',10);"
        "INSERT INTO t(name,val) VALUES('bravo',20);"
        "INSERT INTO t(name,val) VALUES('charlie',30);"
        "COMMIT;", 0, 0, &err);
  check("INSERT 3 rows in a transaction", rc == SQLITE_OK);
  if (err) { snprintf(buf, sizeof buf, "[FAIL] %s\n", err); line(buf); sqlite3_free(err); err = 0; }

  // 5. Query them back with a prepared statement.
  rc = sqlite3_prepare_v2(db, "SELECT id,name,val FROM t ORDER BY id;", -1, &st, 0);
  check("prepare SELECT", rc == SQLITE_OK && st != 0);
  rows = 0; sumval = 0; okrow = 1;
  {
    const char *names[3] = { "alpha", "bravo", "charlie" };
    while (st && sqlite3_step(st) == SQLITE_ROW) {
      int id  = sqlite3_column_int(st, 0);
      const unsigned char *nm = sqlite3_column_text(st, 1);
      int val = sqlite3_column_int(st, 2);
      if (rows < 3 && (!nm || strcmp((const char*)nm, names[rows]) != 0)) okrow = 0;
      snprintf(buf, sizeof buf, "[ROW]  id=%d name=%s val=%d\n",
               id, nm ? (const char*)nm : "(null)", val);
      line(buf);
      rows++; sumval += val;
    }
  }
  sqlite3_finalize(st); st = 0;
  check("read back exactly 3 rows", rows == 3);
  check("row values sum to 60", sumval == 60);
  check("row names match insertion order", okrow == 1);

  // 6. Close.
  rc = sqlite3_close(db); db = 0;
  check("sqlite3_close", rc == SQLITE_OK);

  // 7. Prove a real file exists on disk after close (round-trip to ext2).
  rc = stat(path, &sstat);
  check("the .db file exists on disk after close", rc == 0);
  snprintf(buf, sizeof buf, "[INFO] on-disk %s size = %ld bytes\n", path, (long)sstat.st_size);
  line(buf);
  check("the .db file is non-empty", rc == 0 && sstat.st_size > 0);

  // 8. Reopen the file and re-count: proves the data was persisted to disk, not
  //    merely held in memory for the life of the first connection.
  cnt = -1;
  rc = sqlite3_open(path, &db);
  if (rc == SQLITE_OK &&
      sqlite3_prepare_v2(db, "SELECT count(*) FROM t;", -1, &st, 0) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) cnt = sqlite3_column_int(st, 0);
    sqlite3_finalize(st); st = 0;
  }
  sqlite3_close(db); db = 0;
  snprintf(buf, sizeof buf, "[INFO] reopened row count = %d\n", cnt);
  line(buf);
  check("reopened DB still has 3 rows (persisted to disk)", cnt == 3);

  snprintf(buf, sizeof buf, "=== sqlitchk: %d passed, %d failed ===\n", g_pass, g_fail);
  line(buf);
  line(g_fail == 0 ? "SQLITCHK: ALL PASS\n" : "SQLITCHK: FAILURES\n");
  return g_fail == 0 ? 0 : 1;
}
