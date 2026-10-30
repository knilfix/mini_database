#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
  char *buffer;
  size_t buffer_length;
  ssize_t input_length;
} InputBuffer;

typedef enum { EXECUTE_SUCCESS, EXECUTE_TABLE_FULL } ExecuteResult;

typedef enum {
  META_COMMAND_SUCCESS,
  META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
  PREPARE_SUCCESS,
  PREPARE_SYNTAX_ERROR,
  PREPARE_NEGATIVE_ID,
  PREPARE_STRING_TOO_LONG,
  PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

typedef enum { STATEMENT_INSERT, STATEMENT_SELECT } StatementType;

/* CONSTANTS */
#define PAGE_SIZE 4096u
#define TABLE_MAX_PAGES 100

#define COLUMN_NAME_SIZE 32
#define MAX_COLUMNS 10
#define INT_COLUMN_SIZE 4u
/* Keeps at least 4 rows per page now, and room for several B-tree cells later
 */
#define MAX_ROW_SIZE 512u

#define DB_MAGIC                                                               \
  0x4D494E49u         // "MINI": a file that doesn't start with this isn't ours
#define DB_VERSION 2u // v2: columns carry a type

/* ---------- Schema ---------- */

typedef enum { COL_INT = 1, COL_TEXT = 2 } ColumnType;

/* Stored on disk as-is, so every field is a fixed-width type */
typedef struct {
  uint32_t type;   // ColumnType
  uint32_t offset; // byte offset within a row
  uint32_t size;   // bytes on disk (text: max length + 1 for the NUL)
  char name[COLUMN_NAME_SIZE]; // e.g. "id", "username"
} Column;

/* Page 0 of the file */
typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t num_rows;
  uint32_t num_columns;
  Column columns[MAX_COLUMNS];
} Header;

_Static_assert(sizeof(Header) <= PAGE_SIZE, "Header must fit in one page");

// Custom Callocs and Malloc with error handling
static void *xmalloc(size_t size) {
  void *p = malloc(size);
  if (p == NULL) {
    printf("Out of memory\n");
    exit(EXIT_FAILURE);
  }
  return p;
}

static void *xcalloc(size_t count, size_t size) {
  void *p = calloc(count, size);
  if (p == NULL) {
    printf("Out of memory\n");
    exit(EXIT_FAILURE);
  }
  return p;
}

/* In-memory view of the schema */
typedef struct {
  uint32_t num_columns;
  uint32_t row_size;      // sum of column sizes
  uint32_t rows_per_page; // PAGE_SIZE / row_size
  Column columns[MAX_COLUMNS];
} Schema;

/* A row is just row_size bytes laid out per the schema */
typedef struct {
  StatementType type;
  uint8_t row[MAX_ROW_SIZE]; // only used by insert statement
} Statement;

typedef struct {
  int file_descriptor;
  uint32_t file_length;
  void *pages[TABLE_MAX_PAGES];
} Pager;

typedef struct {
  Pager *pager;
  uint32_t num_rows;
  Schema schema;
} Table;

typedef struct {
  Table *table;
  uint32_t row_num;
  bool end_of_table; // indicates a position one past the last element
} Cursor;

static void set_err(char *err, size_t len, const char *fmt, const char *arg) {
  snprintf(err, len, fmt, arg);
}

/* Appends a column, computing its offset. Used for default, command-line and
   on-disk schemas alike, so all three get the same validation. */
static bool schema_add_column(Schema *s, const char *name, ColumnType type,
                              uint32_t size, char *err, size_t errlen) {
  if (s->num_columns >= MAX_COLUMNS) {
    snprintf(err, errlen, "too many columns (max %d)", MAX_COLUMNS);
    return false;
  }
  size_t name_len = strlen(name);
  if (name_len == 0 || name_len >= COLUMN_NAME_SIZE ||
      isdigit((unsigned char)name[0])) {
    set_err(err, errlen,
            "bad column name '%s' (letters, digits, _; max 31 chars; no "
            "leading digit)",
            name);
    return false;
  }
  for (size_t i = 0; i < name_len; i++) {
    if (!isalnum((unsigned char)name[i]) && name[i] != '_') {
      set_err(err, errlen, "bad column name '%s' (letters, digits, _ only)",
              name);
      return false;
    }
  }
  for (uint32_t i = 0; i < s->num_columns; i++) {
    if (strcmp(s->columns[i].name, name) == 0) {
      set_err(err, errlen, "duplicate column '%s'", name);
      return false;
    }
  }
  if (s->num_columns == 0 && type != COL_INT) {
    set_err(err, errlen,
            "first column ('%s') must be int: it is the primary key", name);
    return false;
  }
  if ((type == COL_INT && size != INT_COLUMN_SIZE) ||
      (type == COL_TEXT && size < 2) || (type != COL_INT && type != COL_TEXT)) {
    set_err(err, errlen, "invalid type/size for column '%s'", name);
    return false;
  }
  if (size > MAX_ROW_SIZE || s->row_size + size > MAX_ROW_SIZE) {
    snprintf(err, errlen, "row too large (max %u bytes)", MAX_ROW_SIZE);
    return false;
  }

  Column *col = &s->columns[s->num_columns];
  memset(col, 0, sizeof(*col));
  col->type = type;
  col->offset = s->row_size;
  col->size = size;
  memcpy(col->name, name, name_len); // rest stays zero from memset
  s->row_size += size;
  s->num_columns++;
  return true;
}

static bool schema_finalize(Schema *s, char *err, size_t errlen) {
  if (s->num_columns == 0 || s->row_size == 0) {
    snprintf(err, errlen, "schema has no columns");
    return false;
  }
  s->rows_per_page = PAGE_SIZE / s->row_size;
  return true;
}

static Schema default_schema(void) {
  printf("No schema provided, using default schema\n");
  Schema s = {0};
  char err[128];
  schema_add_column(&s, "id", COL_INT, INT_COLUMN_SIZE, err, sizeof err);
  schema_add_column(&s, "username", COL_TEXT, 33, err, sizeof err);
  schema_add_column(&s, "email", COL_TEXT, 256, err, sizeof err);
  schema_finalize(&s, err, sizeof err);
  return s;
}

/* Parses "name:int" or "name:text:N" */
static bool schema_parse_spec(Schema *s, const char *spec, char *err,
                              size_t errlen) {
  char buf[128];
  if (strlen(spec) >= sizeof(buf)) {
    snprintf(err, errlen, "column spec too long");
    return false;
  }
  strcpy(buf, spec);

  char *type = strchr(buf, ':');
  if (type == NULL) {
    set_err(err, errlen, "'%s' should look like name:int or name:text:32",
            spec);
    return false;
  }
  *type++ = '\0';
  char *len = strchr(type, ':');
  if (len != NULL) {
    *len++ = '\0';
  }

  if (strcmp(type, "int") == 0) {
    if (len != NULL) {
      set_err(err, errlen, "int takes no length ('%s')", spec);
      return false;
    }
    return schema_add_column(s, buf, COL_INT, INT_COLUMN_SIZE, err, errlen);
  }
  if (strcmp(type, "text") == 0) {
    if (len == NULL) {
      set_err(err, errlen, "text needs a max length, e.g. %s:text:32", buf);
      return false;
    }
    if (!isdigit((unsigned char)*len)) {
      set_err(err, errlen, "bad text length in '%s'", spec);
      return false;
    }
    char *end;
    unsigned long n = strtoul(len, &end, 10);
    if (*end != '\0' || n < 1 || n >= MAX_ROW_SIZE) {
      set_err(err, errlen, "bad text length in '%s'", spec);
      return false;
    }
    return schema_add_column(s, buf, COL_TEXT, (uint32_t)n + 1, err, errlen);
  }
  set_err(err, errlen, "unknown type '%s' (use int or text:N)", type);
  return false;
}

static bool schema_from_args(int n, char **specs, Schema *out, char *err,
                             size_t errlen) {
  memset(out, 0, sizeof(*out));
  for (int i = 0; i < n; i++) {
    if (!schema_parse_spec(out, specs[i], err, errlen)) {
      return false;
    }
  }
  return schema_finalize(out, err, errlen);
}

/* Rebuild + validate a schema read from disk (never trust the file) */
static bool schema_from_header(const Header *h, Schema *out, char *err,
                               size_t errlen) {
  memset(out, 0, sizeof(*out));
  if (h->num_columns == 0 || h->num_columns > MAX_COLUMNS) {
    snprintf(err, errlen, "bad column count");
    return false;
  }
  for (uint32_t i = 0; i < h->num_columns; i++) {
    const Column *c = &h->columns[i];
    if (memchr(c->name, '\0', COLUMN_NAME_SIZE) == NULL) {
      snprintf(err, errlen, "unterminated column name");
      return false;
    }
    if (!schema_add_column(out, c->name, (ColumnType)c->type, c->size, err,
                           errlen)) {
      return false;
    }
    if (out->columns[i].offset != c->offset) {
      snprintf(err, errlen, "bad column offset");
      return false;
    }
  }
  return schema_finalize(out, err, errlen);
}

static bool schema_equal(const Schema *a, const Schema *b) {
  if (a->num_columns != b->num_columns) {
    return false;
  }
  for (uint32_t i = 0; i < a->num_columns; i++) {
    if (a->columns[i].type != b->columns[i].type ||
        a->columns[i].size != b->columns[i].size ||
        strcmp(a->columns[i].name, b->columns[i].name) != 0) {
      return false;
    }
  }
  return true;
}

static void print_schema(const Schema *s) {
  printf("Schema:\n");
  for (uint32_t i = 0; i < s->num_columns; i++) {
    const Column *c = &s->columns[i];
    if (c->type == COL_INT) {
      printf("  - %s : int%s\n", c->name, i == 0 ? " (primary key)" : "");
    } else {
      printf("  - %s : text(%u)\n", c->name, c->size - 1);
    }
  }
  printf("Row size: %u bytes, %u rows per page\n", s->row_size,
         s->rows_per_page);
}

/* ---------- Rows ---------- */

void print_row(const Schema *s, const void *row) {
  printf("(");
  for (uint32_t i = 0; i < s->num_columns; i++) {
    const Column *c = &s->columns[i];
    const char *p = (const char *)row + c->offset;
    if (i > 0) {
      printf(", ");
    }
    if (c->type == COL_INT) {
      int32_t v;
      memcpy(&v, p, sizeof(v));
      printf("%d", v);
    } else {
      printf("%.*s", (int)c->size, p); // stops at NUL, never past the column
    }
  }
  printf(")\n");
}

/* ---------- Pager ---------- */

void *get_page(Pager *pager, uint32_t page_num) {
  if (page_num >= TABLE_MAX_PAGES) {
    printf("Tried to fetch page number out of bounds. %u > %d\n", page_num,
           TABLE_MAX_PAGES);
    exit(EXIT_FAILURE);
  }

  if (pager->pages[page_num] == NULL) {
    // Cache miss. Allocate zeroed memory and load from file
    void *page = xcalloc(PAGE_SIZE, 1);
    uint32_t num_pages = pager->file_length / PAGE_SIZE;

    // we might save a partial page at the end of the file
    if (pager->file_length % PAGE_SIZE) {
      num_pages += 1;
    }

    if (page_num < num_pages) {
      lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
      ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);

      if (bytes_read == -1) {
        printf("Error reading file: %d\n", errno);
        exit(EXIT_FAILURE);
      }
    }

    pager->pages[page_num] = page;
  }

  return pager->pages[page_num];
}

Pager *pager_open(const char *filename) {
  int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);

  if (fd == -1) {
    printf("Unable to open file %s.\n", filename);
    exit(EXIT_FAILURE);
  }

  off_t file_length = lseek(fd, 0, SEEK_END);

  Pager *pager = xmalloc(sizeof(Pager));
  pager->file_descriptor = fd;
  pager->file_length = file_length;

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    pager->pages[i] = NULL;
  }

  return pager;
}

void pager_flush(Pager *pager, uint32_t page_num, uint32_t size) {
  if (pager->pages[page_num] == NULL) {
    printf("Tried to flush a null page.\n");
    exit(EXIT_FAILURE);
  }

  off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);

  if (offset == -1) {
    printf("Error seeking %d\n", errno);
    exit(EXIT_FAILURE);
  }

  ssize_t bytes_written =
      write(pager->file_descriptor, pager->pages[page_num], size);

  if (bytes_written == -1) {
    printf("Error writing: %d\n", errno);
    exit(EXIT_FAILURE);
  }
}

/* ---------- Cursor ---------- */

Cursor *table_start(Table *table) {
  Cursor *cursor = xmalloc(sizeof(Cursor));
  cursor->table = table;
  cursor->row_num = 0;
  cursor->end_of_table = (table->num_rows == 0);

  return cursor;
}

Cursor *table_end(Table *table) {
  Cursor *cursor = xmalloc(sizeof(Cursor));

  cursor->table = table;
  cursor->row_num = table->num_rows;
  cursor->end_of_table = true;

  return cursor;
}

void *cursor_value(Cursor *cursor) {
  Schema *s = &cursor->table->schema;
  uint32_t row_num = cursor->row_num;
  uint32_t page_num = 1 + row_num / s->rows_per_page; // page 0 is the header
  void *page = get_page(cursor->table->pager, page_num);

  uint32_t slot_index = row_num % s->rows_per_page;
  uint32_t byte_offset = slot_index * s->row_size;
  return page + byte_offset;
}

void cursor_advance(Cursor *cursor) {
  cursor->row_num += 1;

  if (cursor->row_num >= cursor->table->num_rows) {
    cursor->end_of_table = true;
  }
}

/* ---------- Table open/close ---------- */

/* Copies the schema + row count into page 0 and writes it out */
static void write_header(Table *table) {
  Header h = {0};
  h.magic = DB_MAGIC;
  h.version = DB_VERSION;
  h.num_rows = table->num_rows;
  h.num_columns = table->schema.num_columns;
  memcpy(h.columns, table->schema.columns, sizeof(h.columns));

  void *page0 = get_page(table->pager, 0);
  memcpy(page0, &h, sizeof(h));
  pager_flush(table->pager, 0, PAGE_SIZE);
}

/* requested: schema given on the command line, or NULL.
   New file: used as the schema (default if NULL).
   Existing file: the stored schema wins; a mismatching request is an error. */
Table *db_open(const char *filename, const Schema *requested) {
  Pager *pager = pager_open(filename);

  Table *table = xmalloc(sizeof(Table));
  table->pager = pager;

  if (pager->file_length == 0) {
    table->schema = requested ? *requested : default_schema();
    table->num_rows = 0;
    write_header(table);
  } else {
    if (pager->file_length < sizeof(Header)) {
      printf("File is too small to be a mini_database file.\n");
      exit(EXIT_FAILURE);
    }

    Header h;
    memcpy(&h, get_page(pager, 0), sizeof(h));

    if (h.magic != DB_MAGIC) {
      printf("Not a mini_database file (or created by an older version).\n");
      exit(EXIT_FAILURE);
    }
    if (h.version != DB_VERSION) {
      printf("Unsupported database version %u.\n", h.version);
      exit(EXIT_FAILURE);
    }

    char err[160];
    Schema loaded;
    if (!schema_from_header(&h, &loaded, err, sizeof err)) {
      printf("Corrupt schema in %s: %s\n", filename, err);
      exit(EXIT_FAILURE);
    }
    if (requested && !schema_equal(requested, &loaded)) {
      printf("The schema you gave doesn't match the one stored in %s.\n"
             "Run without schema arguments to use the stored one.\n",
             filename);
      exit(EXIT_FAILURE);
    }
    if (h.num_rows > loaded.rows_per_page * (TABLE_MAX_PAGES - 1)) {
      printf("Corrupt row count in %s.\n", filename);
      exit(EXIT_FAILURE);
    }

    table->schema = loaded;
    table->num_rows = h.num_rows;
  }

  return table;
}

void db_close(Table *table) {
  Pager *pager = table->pager;
  Schema *s = &table->schema;

  // 1. Header (page 0) carries the row count, so rewrite it on exit
  write_header(table);
  free(pager->pages[0]);
  pager->pages[0] = NULL;

  // 2. Full row pages. Row pages start at 1
  uint32_t num_full_pages = table->num_rows / s->rows_per_page;
  for (uint32_t i = 1; i <= num_full_pages; i++) {
    if (pager->pages[i] == NULL) {
      continue;
    }
    pager_flush(pager, i, PAGE_SIZE);
    free(pager->pages[i]);
    pager->pages[i] = NULL;
  }

  // 3. Partial last page, which lives at 1 + num_full_pages
  uint32_t num_of_additional_rows = table->num_rows % s->rows_per_page;
  if (num_of_additional_rows > 0) {
    uint32_t page_num = 1 + num_full_pages;
    if (pager->pages[page_num] != NULL) {
      pager_flush(pager, page_num, num_of_additional_rows * s->row_size);
      free(pager->pages[page_num]);
      pager->pages[page_num] = NULL;
    }
  }

  // 4. Close fd
  int result = close(pager->file_descriptor);
  if (result == -1) {
    printf("Error closing db file.\n");
    exit(EXIT_FAILURE);
  }

  // 5. Free any still-cached pages (e.g. pages read but never written)
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    void *page = pager->pages[i];
    if (page) {
      free(page);
      pager->pages[i] = NULL;
    }
  }
  free(pager);
  free(table);
}

/* ---------- Input ---------- */

InputBuffer *new_input_buffer(void) {
  InputBuffer *input_buffer = xmalloc(sizeof(InputBuffer));

  input_buffer->buffer = NULL;
  input_buffer->buffer_length = 0;
  input_buffer->input_length = 0;

  return input_buffer;
}

void print_prompt(void) { printf("db > "); }

bool read_input(InputBuffer *input_buffer) {
  ssize_t bytes_read =
      getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);

  if (bytes_read == -1) {
    if (feof(stdin)) {
      return false; // clean EOF: caller shuts down normally
    }
    printf("Error reading input: %d\n", errno);
    exit(EXIT_FAILURE);
  }

  if (input_buffer->buffer[bytes_read - 1] == '\n') {
    // Ignore trailing newline
    input_buffer->input_length = bytes_read - 1;
    input_buffer->buffer[bytes_read - 1] = 0;
    return true;
  } else {
    input_buffer->input_length = bytes_read;
    return true;
  }
}

void close_input_buffer(InputBuffer *input_buffer) {
  free(input_buffer->buffer);
  free(input_buffer);
}

/* ---------- Commands ---------- */

MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table) {
  if (strcmp(input_buffer->buffer, ".exit") == 0) {
    close_input_buffer(input_buffer);
    db_close(table);
    exit(EXIT_SUCCESS);
  } else if (strcmp(input_buffer->buffer, ".schema") == 0) {
    print_schema(&table->schema);
    return META_COMMAND_SUCCESS;
  } else {
    return META_COMMAND_UNRECOGNIZED_COMMAND;
  }
}

/* insert <col0> <col1> ... : one value per schema column, parsed straight
   into the on-disk row layout */
PrepareResult prepare_insert(InputBuffer *input_buffer, Statement *statement,
                             const Schema *s) {
  statement->type = STATEMENT_INSERT;
  memset(statement->row, 0, sizeof(statement->row)); // no stale bytes on disk

  strtok(input_buffer->buffer, " "); // the "insert" keyword

  for (uint32_t i = 0; i < s->num_columns; i++) {
    const Column *c = &s->columns[i];
    char *token = strtok(NULL, " ");
    if (token == NULL) {
      return PREPARE_SYNTAX_ERROR;
    }

    if (c->type == COL_INT) {
      char *end;
      errno = 0;
      long v = strtol(token, &end, 10);
      if (end == token || *end != '\0' || errno == ERANGE || v < INT32_MIN ||
          v > INT32_MAX) {
        return PREPARE_SYNTAX_ERROR;
      }
      if (i == 0 && v < 0) {
        return PREPARE_NEGATIVE_ID; // column 0 is the key
      }
      int32_t value = (int32_t)v;
      memcpy(statement->row + c->offset, &value, sizeof(value));
    } else {
      size_t len = strlen(token);
      if (len > c->size - 1) {
        return PREPARE_STRING_TOO_LONG;
      }
      memcpy(statement->row + c->offset, token, len);
    }
  }

  if (strtok(NULL, " ") != NULL) {
    return PREPARE_SYNTAX_ERROR; // too many values
  }
  return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer *input_buffer, Statement *statement,
                                const Schema *s) {
  // Handle insert
  if (strcmp(input_buffer->buffer, "insert") == 0 ||
      strncmp(input_buffer->buffer, "insert ", 7) == 0) {
    return prepare_insert(input_buffer, statement, s);
  }
  // Handle Select
  if (strcmp(input_buffer->buffer, "select") == 0) {
    statement->type = STATEMENT_SELECT;
    return PREPARE_SUCCESS;
  }

  return PREPARE_UNRECOGNIZED_STATEMENT;
}

ExecuteResult execute_insert(Statement *statement, Table *table) {
  Schema *s = &table->schema;
  uint32_t table_max_rows = s->rows_per_page * (TABLE_MAX_PAGES - 1);
  if (table->num_rows >= table_max_rows) {
    return EXECUTE_TABLE_FULL;
  }

  Cursor *cursor = table_end(table);

  memcpy(cursor_value(cursor), statement->row, s->row_size);
  table->num_rows += 1;
  free(cursor);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement *statement, Table *table) {
  Cursor *cursor = table_start(table);
  (void)statement;
  while (!(cursor->end_of_table)) {
    print_row(&table->schema, cursor_value(cursor));
    cursor_advance(cursor);
  }
  free(cursor);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement *statement, Table *table) {
  switch (statement->type) {
  case STATEMENT_INSERT:
    return execute_insert(statement, table);

  case STATEMENT_SELECT:
    return execute_select(statement, table);
  }
  return EXECUTE_SUCCESS;
}

int main(int argc, char *argv[]) {

  if (argc < 2) {
    printf("Must supply a database filename.\n");
    printf("Usage: %s <file> [name:type ...]\n", argv[0]);
    printf("  types: int | text:<max length>\n");
    printf("  first column must be int (primary key); default schema is\n"
           "  id:int username:text:32 email:text:255\n");
    exit(EXIT_FAILURE);
  }

  char *filename = argv[1];

  // Optional schema from the command line (only used when creating the file)
  Schema requested;
  bool has_requested = false;
  if (argc > 2) {
    char err[160];
    if (!schema_from_args(argc - 2, argv + 2, &requested, err, sizeof err)) {
      printf("Invalid schema: %s\n", err);
      exit(EXIT_FAILURE);
    }
    has_requested = true;
  }

  // Initialize the Table
  Table *table = db_open(filename, has_requested ? &requested : NULL);

  InputBuffer *input_buffer = new_input_buffer();
  while (true) {
    print_prompt();

    if (!read_input(input_buffer)) {
      if (isatty(STDIN_FILENO)) {
        printf("\n"); // so the shell prompt doesn't land on the "db > " line
      }
      close_input_buffer(input_buffer);
      db_close(table);
      exit(EXIT_SUCCESS);
    }

    /* Handle meta commands */
    if (input_buffer->buffer[0] == '.') {
      switch (do_meta_command(input_buffer, table)) {
      case META_COMMAND_SUCCESS:
        continue;
      case META_COMMAND_UNRECOGNIZED_COMMAND:
        printf("Unrecognized command '%s'.\n", input_buffer->buffer);
        continue;
      }
    }

    /* Handle SQL statements */
    Statement statement = {0};
    switch (prepare_statement(input_buffer, &statement, &table->schema)) {
    case PREPARE_SUCCESS:
      break;
    case PREPARE_NEGATIVE_ID:
      printf("ID must be non-negative.\n");
      continue;
    case PREPARE_STRING_TOO_LONG:
      printf("String is too long.\n");
      continue;
    case PREPARE_SYNTAX_ERROR:
      printf("Syntax error. Could not parse statement.\n");
      continue;

    case PREPARE_UNRECOGNIZED_STATEMENT:
      printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
      continue;
    }

    switch (execute_statement(&statement, table)) {
    case (EXECUTE_SUCCESS):
      printf("Executed.\n");
      break;
    case (EXECUTE_TABLE_FULL):
      printf("Error: Table full.\n");
      break;
    }
  }
}
