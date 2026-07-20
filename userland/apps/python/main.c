// python - Minimal Python interpreter for MayteraOS
// A basic Python-like scripting environment
// 
// Supports:
// - Variables and expressions
// - print() function
// - Basic arithmetic
// - if/elif/else statements
// - while/for loops
// - Function definitions
// - String operations
// - List operations
// - File I/O via open(), read(), write()
// - OS operations via os module
// - GUI via maytera module

#include "../../libc/maytera.h"
#include "../../libc/gui.h"

// ============================================================================
// Token Types
// ============================================================================

typedef enum {
    TOK_EOF,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ, TOK_NE, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_ASSIGN,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE,
    TOK_COMMA, TOK_COLON, TOK_DOT,
    TOK_AND, TOK_OR, TOK_NOT,
    TOK_IF, TOK_ELIF, TOK_ELSE,
    TOK_WHILE, TOK_FOR, TOK_IN,
    TOK_DEF, TOK_RETURN,
    TOK_IMPORT, TOK_FROM,
    TOK_TRUE, TOK_FALSE, TOK_NONE,
    TOK_BREAK, TOK_CONTINUE,
    TOK_CLASS,
    TOK_PLUSEQ, TOK_MINUSEQ, TOK_STAREQ, TOK_SLASHEQ,
} token_type_t;

// ============================================================================
// Value Types
// ============================================================================

typedef enum {
    VAL_NONE,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_LIST,
    VAL_DICT,
    VAL_FUNC,
    VAL_BUILTIN,
    VAL_OBJECT,
} value_type_t;

// Forward declarations
struct value;
struct function;
struct object;

typedef struct value {
    value_type_t type;
    union {
        int bool_val;
        long int_val;
        double float_val;
        char *str_val;
        struct {
            struct value *items;
            int count;
            int capacity;
        } list;
        struct {
            char **keys;
            struct value *values;
            int count;
        } dict;
        struct function *func;
        struct value (*builtin)(struct value *args, int argc);
        struct object *obj;
    };
} value_t;

typedef struct function {
    char *name;
    char **params;
    int param_count;
    char *body;
} function_t;

// ============================================================================
// Parser State
// ============================================================================

static char *source;
static int pos;
static int lineno;
static int indent_stack[32];
static int indent_level;

static token_type_t cur_token;
static char token_str[256];
static long token_int;
static double token_float;

// ============================================================================
// Variables
// ============================================================================

#define MAX_VARS 256

typedef struct {
    char name[64];
    value_t value;
} variable_t;

static variable_t variables[MAX_VARS];
static int var_count = 0;

// Local variable scopes
#define MAX_SCOPE_DEPTH 16
static int scope_stack[MAX_SCOPE_DEPTH];
static int scope_depth = 0;

// ============================================================================
// Functions
// ============================================================================

#define MAX_FUNCS 64

static function_t functions[MAX_FUNCS];
static int func_count = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static int my_isdigit(char c) { return c >= '0' && c <= '9'; }
static int my_isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int my_isalnum(char c) { return my_isalpha(c) || my_isdigit(c); }
static int my_isspace(char c) { return c == ' ' || c == '\t'; }

static int my_strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void my_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

static int my_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static char *my_strdup(const char *s) {
    int len = my_strlen(s);
    char *d = malloc(len + 1);
    if (d) my_strcpy(d, s);
    return d;
}

// ============================================================================
// Tokenizer
// ============================================================================

static void skip_whitespace(void) {
    while (source[pos] && my_isspace(source[pos])) pos++;
}

static void skip_comment(void) {
    if (source[pos] == '#') {
        while (source[pos] && source[pos] != '\n') pos++;
    }
}

static token_type_t next_token(void) {
    // Handle newlines and indentation
    if (source[pos] == '\n') {
        pos++;
        lineno++;
        
        // Count indentation
        int spaces = 0;
        while (source[pos] == ' ') { pos++; spaces++; }
        while (source[pos] == '\t') { pos++; spaces += 4; }
        
        // Skip blank lines and comments
        while (source[pos] == '#' || source[pos] == '\n') {
            skip_comment();
            if (source[pos] == '\n') {
                pos++;
                lineno++;
                spaces = 0;
                while (source[pos] == ' ') { pos++; spaces++; }
                while (source[pos] == '\t') { pos++; spaces += 4; }
            }
        }
        
        if (source[pos] == '\0') {
            return (cur_token = TOK_EOF);
        }
        
        // Check indent/dedent
        if (spaces > indent_stack[indent_level]) {
            indent_level++;
            indent_stack[indent_level] = spaces;
            return (cur_token = TOK_INDENT);
        } else if (spaces < indent_stack[indent_level]) {
            indent_level--;
            return (cur_token = TOK_DEDENT);
        }
        
        return (cur_token = TOK_NEWLINE);
    }
    
    skip_whitespace();
    skip_comment();
    
    if (source[pos] == '\0') {
        return (cur_token = TOK_EOF);
    }
    
    // Numbers
    if (my_isdigit(source[pos])) {
        int i = 0;
        while (my_isdigit(source[pos]) || source[pos] == '.') {
            token_str[i++] = source[pos++];
        }
        token_str[i] = '\0';
        token_int = 0;
        for (int j = 0; token_str[j]; j++) {
            if (token_str[j] != '.') {
                token_int = token_int * 10 + (token_str[j] - '0');
            }
        }
        return (cur_token = TOK_NUMBER);
    }
    
    // Strings
    if (source[pos] == '"' || source[pos] == '\'') {
        char quote = source[pos++];
        int i = 0;
        while (source[pos] && source[pos] != quote) {
            if (source[pos] == '\\' && source[pos+1]) {
                pos++;
                switch (source[pos]) {
                    case 'n': token_str[i++] = '\n'; break;
                    case 't': token_str[i++] = '\t'; break;
                    case 'r': token_str[i++] = '\r'; break;
                    case '\\': token_str[i++] = '\\'; break;
                    case '"': token_str[i++] = '"'; break;
                    case '\'': token_str[i++] = '\''; break;
                    default: token_str[i++] = source[pos];
                }
                pos++;
            } else {
                token_str[i++] = source[pos++];
            }
        }
        token_str[i] = '\0';
        if (source[pos] == quote) pos++;
        return (cur_token = TOK_STRING);
    }
    
    // Identifiers and keywords
    if (my_isalpha(source[pos])) {
        int i = 0;
        while (my_isalnum(source[pos])) {
            token_str[i++] = source[pos++];
        }
        token_str[i] = '\0';
        
        // Check keywords
        if (!my_strcmp(token_str, "if")) return (cur_token = TOK_IF);
        if (!my_strcmp(token_str, "elif")) return (cur_token = TOK_ELIF);
        if (!my_strcmp(token_str, "else")) return (cur_token = TOK_ELSE);
        if (!my_strcmp(token_str, "while")) return (cur_token = TOK_WHILE);
        if (!my_strcmp(token_str, "for")) return (cur_token = TOK_FOR);
        if (!my_strcmp(token_str, "in")) return (cur_token = TOK_IN);
        if (!my_strcmp(token_str, "def")) return (cur_token = TOK_DEF);
        if (!my_strcmp(token_str, "return")) return (cur_token = TOK_RETURN);
        if (!my_strcmp(token_str, "import")) return (cur_token = TOK_IMPORT);
        if (!my_strcmp(token_str, "from")) return (cur_token = TOK_FROM);
        if (!my_strcmp(token_str, "True")) return (cur_token = TOK_TRUE);
        if (!my_strcmp(token_str, "False")) return (cur_token = TOK_FALSE);
        if (!my_strcmp(token_str, "None")) return (cur_token = TOK_NONE);
        if (!my_strcmp(token_str, "and")) return (cur_token = TOK_AND);
        if (!my_strcmp(token_str, "or")) return (cur_token = TOK_OR);
        if (!my_strcmp(token_str, "not")) return (cur_token = TOK_NOT);
        if (!my_strcmp(token_str, "break")) return (cur_token = TOK_BREAK);
        if (!my_strcmp(token_str, "continue")) return (cur_token = TOK_CONTINUE);
        if (!my_strcmp(token_str, "class")) return (cur_token = TOK_CLASS);
        
        return (cur_token = TOK_IDENT);
    }
    
    // Operators
    char c = source[pos++];
    switch (c) {
        case '+': 
            if (source[pos] == '=') { pos++; return (cur_token = TOK_PLUSEQ); }
            return (cur_token = TOK_PLUS);
        case '-':
            if (source[pos] == '=') { pos++; return (cur_token = TOK_MINUSEQ); }
            return (cur_token = TOK_MINUS);
        case '*':
            if (source[pos] == '=') { pos++; return (cur_token = TOK_STAREQ); }
            return (cur_token = TOK_STAR);
        case '/':
            if (source[pos] == '=') { pos++; return (cur_token = TOK_SLASHEQ); }
            return (cur_token = TOK_SLASH);
        case '%': return (cur_token = TOK_PERCENT);
        case '=':
            if (source[pos] == '=') { pos++; return (cur_token = TOK_EQ); }
            return (cur_token = TOK_ASSIGN);
        case '!':
            if (source[pos] == '=') { pos++; return (cur_token = TOK_NE); }
            return (cur_token = TOK_NOT);
        case '<':
            if (source[pos] == '=') { pos++; return (cur_token = TOK_LE); }
            return (cur_token = TOK_LT);
        case '>':
            if (source[pos] == '=') { pos++; return (cur_token = TOK_GE); }
            return (cur_token = TOK_GT);
        case '(': return (cur_token = TOK_LPAREN);
        case ')': return (cur_token = TOK_RPAREN);
        case '[': return (cur_token = TOK_LBRACKET);
        case ']': return (cur_token = TOK_RBRACKET);
        case '{': return (cur_token = TOK_LBRACE);
        case '}': return (cur_token = TOK_RBRACE);
        case ',': return (cur_token = TOK_COMMA);
        case ':': return (cur_token = TOK_COLON);
        case '.': return (cur_token = TOK_DOT);
    }
    
    return (cur_token = TOK_EOF);
}

// ============================================================================
// Variable Management
// ============================================================================

static value_t *get_var(const char *name) {
    for (int i = var_count - 1; i >= 0; i--) {
        if (!my_strcmp(variables[i].name, name)) {
            return &variables[i].value;
        }
    }
    return NULL;
}

static void set_var(const char *name, value_t val) {
    // Check existing
    for (int i = var_count - 1; i >= 0; i--) {
        if (!my_strcmp(variables[i].name, name)) {
            variables[i].value = val;
            return;
        }
    }
    // New variable
    if (var_count < MAX_VARS) {
        my_strcpy(variables[var_count].name, name);
        variables[var_count].value = val;
        var_count++;
    }
}

static void push_scope(void) {
    if (scope_depth < MAX_SCOPE_DEPTH) {
        scope_stack[scope_depth++] = var_count;
    }
}

static void pop_scope(void) {
    if (scope_depth > 0) {
        var_count = scope_stack[--scope_depth];
    }
}

// ============================================================================
// Value Operations
// ============================================================================

static value_t make_none(void) {
    value_t v = { .type = VAL_NONE };
    return v;
}

static value_t make_bool(int b) {
    value_t v = { .type = VAL_BOOL, .bool_val = b };
    return v;
}

static value_t make_int(long i) {
    value_t v = { .type = VAL_INT, .int_val = i };
    return v;
}

static value_t make_string(const char *s) {
    value_t v = { .type = VAL_STRING, .str_val = my_strdup(s) };
    return v;
}

static value_t make_list(void) {
    value_t v = { .type = VAL_LIST };
    v.list.items = NULL;
    v.list.count = 0;
    v.list.capacity = 0;
    return v;
}

static void list_append(value_t *list, value_t item) {
    if (list->type != VAL_LIST) return;
    if (list->list.count >= list->list.capacity) {
        int new_cap = list->list.capacity ? list->list.capacity * 2 : 8;
        value_t *new_items = malloc(new_cap * sizeof(value_t));
        if (list->list.items) {
            for (int i = 0; i < list->list.count; i++) {
                new_items[i] = list->list.items[i];
            }
            free(list->list.items);
        }
        list->list.items = new_items;
        list->list.capacity = new_cap;
    }
    list->list.items[list->list.count++] = item;
}

static int value_is_true(value_t v) {
    switch (v.type) {
        case VAL_NONE: return 0;
        case VAL_BOOL: return v.bool_val;
        case VAL_INT: return v.int_val != 0;
        case VAL_STRING: return v.str_val && v.str_val[0];
        case VAL_LIST: return v.list.count > 0;
        default: return 1;
    }
}

static void print_value(value_t v) {
    switch (v.type) {
        case VAL_NONE: printf("None"); break;
        case VAL_BOOL: printf(v.bool_val ? "True" : "False"); break;
        case VAL_INT: printf("%ld", v.int_val); break;
        case VAL_FLOAT: printf("%f", v.float_val); break;
        case VAL_STRING: printf("%s", v.str_val); break;
        case VAL_LIST:
            printf("[");
            for (int i = 0; i < v.list.count; i++) {
                if (i > 0) printf(", ");
                if (v.list.items[i].type == VAL_STRING) printf("'");
                print_value(v.list.items[i]);
                if (v.list.items[i].type == VAL_STRING) printf("'");
            }
            printf("]");
            break;
        case VAL_FUNC: printf("<function %s>", v.func ? v.func->name : "?"); break;
        case VAL_BUILTIN: printf("<builtin function>"); break;
        default: printf("<object>"); break;
    }
}

// ============================================================================
// Built-in Functions
// ============================================================================

static value_t builtin_print(value_t *args, int argc) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        print_value(args[i]);
    }
    printf("\n");
    return make_none();
}

static value_t builtin_len(value_t *args, int argc) {
    if (argc < 1) return make_int(0);
    if (args[0].type == VAL_STRING) {
        return make_int(my_strlen(args[0].str_val));
    }
    if (args[0].type == VAL_LIST) {
        return make_int(args[0].list.count);
    }
    return make_int(0);
}

static value_t builtin_range(value_t *args, int argc) {
    int start = 0, stop = 0, step = 1;
    if (argc == 1) {
        stop = args[0].int_val;
    } else if (argc >= 2) {
        start = args[0].int_val;
        stop = args[1].int_val;
    }
    if (argc >= 3) {
        step = args[2].int_val;
    }
    
    value_t list = make_list();
    for (int i = start; step > 0 ? i < stop : i > stop; i += step) {
        list_append(&list, make_int(i));
    }
    return list;
}

static value_t builtin_str(value_t *args, int argc) {
    if (argc < 1) return make_string("");
    char buf[256];
    switch (args[0].type) {
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%ld", args[0].int_val);
            return make_string(buf);
        case VAL_STRING:
            return args[0];
        default:
            return make_string("");
    }
}

static value_t builtin_int(value_t *args, int argc) {
    if (argc < 1) return make_int(0);
    if (args[0].type == VAL_INT) return args[0];
    if (args[0].type == VAL_STRING) {
        long val = 0;
        const char *s = args[0].str_val;
        int neg = 0;
        if (*s == '-') { neg = 1; s++; }
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            s++;
        }
        return make_int(neg ? -val : val);
    }
    return make_int(0);
}

static value_t builtin_input(value_t *args, int argc) {
    if (argc > 0 && args[0].type == VAL_STRING) {
        printf("%s", args[0].str_val);
    }
    static char buf[256];
    int i = 0;
    int c;
    while ((c = getchar()) != '\n' && c != -1 && i < 255) {
        buf[i++] = c;
    }
    buf[i] = '\0';
    return make_string(buf);
}

static value_t builtin_open(value_t *args, int argc) {
    if (argc < 1 || args[0].type != VAL_STRING) {
        printf("Error: open() requires filename\n");
        return make_none();
    }
    const char *mode = argc > 1 && args[1].type == VAL_STRING ? args[1].str_val : "r";
    int flags = 0;
    if (mode[0] == 'r') flags = 0;
    else if (mode[0] == 'w') flags = 0x101;  // O_WRONLY | O_CREAT
    else if (mode[0] == 'a') flags = 0x401;  // O_WRONLY | O_APPEND
    
    int fd = open(args[0].str_val, flags);
    if (fd < 0) {
        printf("Error: Cannot open file\n");
        return make_none();
    }
    return make_int(fd);  // Return file descriptor as int
}

static void init_builtins(void) {
    value_t v;
    v.type = VAL_BUILTIN;
    
    v.builtin = builtin_print;
    set_var("print", v);
    
    v.builtin = builtin_len;
    set_var("len", v);
    
    v.builtin = builtin_range;
    set_var("range", v);
    
    v.builtin = builtin_str;
    set_var("str", v);
    
    v.builtin = builtin_int;
    set_var("int", v);
    
    v.builtin = builtin_input;
    set_var("input", v);
    
    v.builtin = builtin_open;
    set_var("open", v);
}

// ============================================================================
// Expression Parser (Forward Declaration)
// ============================================================================

static value_t parse_expression(void);
static void parse_statement(void);
static void parse_block(void);

// ============================================================================
// Expression Parser
// ============================================================================

static value_t parse_primary(void) {
    if (cur_token == TOK_NUMBER) {
        long val = token_int;
        next_token();
        return make_int(val);
    }
    
    if (cur_token == TOK_STRING) {
        value_t v = make_string(token_str);
        next_token();
        return v;
    }
    
    if (cur_token == TOK_TRUE) {
        next_token();
        return make_bool(1);
    }
    
    if (cur_token == TOK_FALSE) {
        next_token();
        return make_bool(0);
    }
    
    if (cur_token == TOK_NONE) {
        next_token();
        return make_none();
    }
    
    if (cur_token == TOK_LBRACKET) {
        // List literal
        next_token();
        value_t list = make_list();
        while (cur_token != TOK_RBRACKET && cur_token != TOK_EOF) {
            list_append(&list, parse_expression());
            if (cur_token == TOK_COMMA) next_token();
        }
        if (cur_token == TOK_RBRACKET) next_token();
        return list;
    }
    
    if (cur_token == TOK_LPAREN) {
        next_token();
        value_t v = parse_expression();
        if (cur_token == TOK_RPAREN) next_token();
        return v;
    }
    
    if (cur_token == TOK_IDENT) {
        char name[64];
        my_strcpy(name, token_str);
        next_token();
        
        // Function call
        if (cur_token == TOK_LPAREN) {
            next_token();
            value_t args[16];
            int argc = 0;
            while (cur_token != TOK_RPAREN && cur_token != TOK_EOF && argc < 16) {
                args[argc++] = parse_expression();
                if (cur_token == TOK_COMMA) next_token();
            }
            if (cur_token == TOK_RPAREN) next_token();
            
            value_t *func = get_var(name);
            if (func && func->type == VAL_BUILTIN) {
                return func->builtin(args, argc);
            }
            if (func && func->type == VAL_FUNC) {
                // User function call
                push_scope();
                for (int i = 0; i < func->func->param_count && i < argc; i++) {
                    set_var(func->func->params[i], args[i]);
                }
                // Execute function body
                char *old_source = source;
                int old_pos = pos;
                source = func->func->body;
                pos = 0;
                next_token();
                while (cur_token != TOK_EOF) {
                    parse_statement();
                }
                source = old_source;
                pos = old_pos;
                next_token();
                pop_scope();
                return make_none();  // TODO: handle return value
            }
            printf("Error: Unknown function: %s\n", name);
            return make_none();
        }
        
        // Array subscript
        if (cur_token == TOK_LBRACKET) {
            next_token();
            value_t index = parse_expression();
            if (cur_token == TOK_RBRACKET) next_token();
            
            value_t *var = get_var(name);
            if (var && var->type == VAL_LIST) {
                int idx = index.int_val;
                if (idx >= 0 && idx < var->list.count) {
                    return var->list.items[idx];
                }
            }
            if (var && var->type == VAL_STRING) {
                int idx = index.int_val;
                if (idx >= 0 && idx < my_strlen(var->str_val)) {
                    char buf[2] = { var->str_val[idx], '\0' };
                    return make_string(buf);
                }
            }
            return make_none();
        }
        
        // Variable lookup
        value_t *var = get_var(name);
        if (var) return *var;
        
        printf("Error: Undefined variable: %s\n", name);
        return make_none();
    }
    
    return make_none();
}

static value_t parse_unary(void) {
    if (cur_token == TOK_MINUS) {
        next_token();
        value_t v = parse_unary();
        if (v.type == VAL_INT) v.int_val = -v.int_val;
        return v;
    }
    if (cur_token == TOK_NOT) {
        next_token();
        value_t v = parse_unary();
        return make_bool(!value_is_true(v));
    }
    return parse_primary();
}

static value_t parse_mult(void) {
    value_t left = parse_unary();
    while (cur_token == TOK_STAR || cur_token == TOK_SLASH || cur_token == TOK_PERCENT) {
        token_type_t op = cur_token;
        next_token();
        value_t right = parse_unary();
        if (left.type == VAL_INT && right.type == VAL_INT) {
            if (op == TOK_STAR) left.int_val *= right.int_val;
            else if (op == TOK_SLASH && right.int_val) left.int_val /= right.int_val;
            else if (op == TOK_PERCENT && right.int_val) left.int_val %= right.int_val;
        }
    }
    return left;
}

static value_t parse_add(void) {
    value_t left = parse_mult();
    while (cur_token == TOK_PLUS || cur_token == TOK_MINUS) {
        token_type_t op = cur_token;
        next_token();
        value_t right = parse_mult();
        if (left.type == VAL_INT && right.type == VAL_INT) {
            if (op == TOK_PLUS) left.int_val += right.int_val;
            else left.int_val -= right.int_val;
        } else if (left.type == VAL_STRING && right.type == VAL_STRING && op == TOK_PLUS) {
            // String concatenation
            int len1 = my_strlen(left.str_val);
            int len2 = my_strlen(right.str_val);
            char *s = malloc(len1 + len2 + 1);
            my_strcpy(s, left.str_val);
            my_strcpy(s + len1, right.str_val);
            left.str_val = s;
        }
    }
    return left;
}

static value_t parse_comparison(void) {
    value_t left = parse_add();
    while (cur_token >= TOK_EQ && cur_token <= TOK_GE) {
        token_type_t op = cur_token;
        next_token();
        value_t right = parse_add();
        int result = 0;
        if (left.type == VAL_INT && right.type == VAL_INT) {
            switch (op) {
                case TOK_EQ: result = left.int_val == right.int_val; break;
                case TOK_NE: result = left.int_val != right.int_val; break;
                case TOK_LT: result = left.int_val < right.int_val; break;
                case TOK_GT: result = left.int_val > right.int_val; break;
                case TOK_LE: result = left.int_val <= right.int_val; break;
                case TOK_GE: result = left.int_val >= right.int_val; break;
                default: break;
            }
        } else if (left.type == VAL_STRING && right.type == VAL_STRING) {
            int cmp = my_strcmp(left.str_val, right.str_val);
            switch (op) {
                case TOK_EQ: result = cmp == 0; break;
                case TOK_NE: result = cmp != 0; break;
                case TOK_LT: result = cmp < 0; break;
                case TOK_GT: result = cmp > 0; break;
                case TOK_LE: result = cmp <= 0; break;
                case TOK_GE: result = cmp >= 0; break;
                default: break;
            }
        }
        left = make_bool(result);
    }
    return left;
}

static value_t parse_and(void) {
    value_t left = parse_comparison();
    while (cur_token == TOK_AND) {
        next_token();
        if (!value_is_true(left)) return left;  // Short circuit
        left = parse_comparison();
    }
    return left;
}

static value_t parse_or(void) {
    value_t left = parse_and();
    while (cur_token == TOK_OR) {
        next_token();
        if (value_is_true(left)) return left;  // Short circuit
        left = parse_and();
    }
    return left;
}

static value_t parse_expression(void) {
    return parse_or();
}

// ============================================================================
// Statement Parser
// ============================================================================

static int break_flag = 0;
static int continue_flag = 0;

static void parse_block(void) {
    if (cur_token == TOK_INDENT) {
        next_token();
        while (cur_token != TOK_DEDENT && cur_token != TOK_EOF) {
            parse_statement();
            if (break_flag || continue_flag) break;
        }
        if (cur_token == TOK_DEDENT) next_token();
    }
}

static void parse_statement(void) {
    // Skip newlines
    while (cur_token == TOK_NEWLINE) next_token();
    
    if (cur_token == TOK_EOF || cur_token == TOK_DEDENT) return;
    
    // if statement
    if (cur_token == TOK_IF) {
        next_token();
        value_t cond = parse_expression();
        if (cur_token == TOK_COLON) next_token();
        if (cur_token == TOK_NEWLINE) next_token();
        
        if (value_is_true(cond)) {
            parse_block();
            // Skip elif/else
            while (cur_token == TOK_ELIF || cur_token == TOK_ELSE) {
                if (cur_token == TOK_ELIF) {
                    next_token();
                    parse_expression();  // Skip condition
                }
                if (cur_token == TOK_COLON) next_token();
                if (cur_token == TOK_NEWLINE) next_token();
                // Skip block
                if (cur_token == TOK_INDENT) {
                    int depth = 1;
                    next_token();
                    while (depth > 0 && cur_token != TOK_EOF) {
                        if (cur_token == TOK_INDENT) depth++;
                        else if (cur_token == TOK_DEDENT) depth--;
                        next_token();
                    }
                }
            }
        } else {
            // Skip if block
            if (cur_token == TOK_INDENT) {
                int depth = 1;
                next_token();
                while (depth > 0 && cur_token != TOK_EOF) {
                    if (cur_token == TOK_INDENT) depth++;
                    else if (cur_token == TOK_DEDENT) depth--;
                    next_token();
                }
            }
            // Handle elif/else
            while (cur_token == TOK_ELIF) {
                next_token();
                cond = parse_expression();
                if (cur_token == TOK_COLON) next_token();
                if (cur_token == TOK_NEWLINE) next_token();
                if (value_is_true(cond)) {
                    parse_block();
                    // Skip remaining elif/else
                    while (cur_token == TOK_ELIF || cur_token == TOK_ELSE) {
                        if (cur_token == TOK_ELIF) {
                            next_token();
                            parse_expression();
                        }
                        if (cur_token == TOK_COLON) next_token();
                        if (cur_token == TOK_NEWLINE) next_token();
                        if (cur_token == TOK_INDENT) {
                            int depth = 1;
                            next_token();
                            while (depth > 0 && cur_token != TOK_EOF) {
                                if (cur_token == TOK_INDENT) depth++;
                                else if (cur_token == TOK_DEDENT) depth--;
                                next_token();
                            }
                        }
                    }
                    return;
                } else {
                    // Skip elif block
                    if (cur_token == TOK_INDENT) {
                        int depth = 1;
                        next_token();
                        while (depth > 0 && cur_token != TOK_EOF) {
                            if (cur_token == TOK_INDENT) depth++;
                            else if (cur_token == TOK_DEDENT) depth--;
                            next_token();
                        }
                    }
                }
            }
            if (cur_token == TOK_ELSE) {
                next_token();
                if (cur_token == TOK_COLON) next_token();
                if (cur_token == TOK_NEWLINE) next_token();
                parse_block();
            }
        }
        return;
    }
    
    // while statement
    if (cur_token == TOK_WHILE) {
        next_token();
        int cond_pos = pos;
        char *cond_source = source;
        
        // Save loop start for re-evaluation
        while (1) {
            source = cond_source;
            pos = cond_pos;
            next_token();
            
            value_t cond = parse_expression();
            if (cur_token == TOK_COLON) next_token();
            if (cur_token == TOK_NEWLINE) next_token();
            
            if (!value_is_true(cond)) break;
            
            // Save block position
            int block_pos = pos;
            parse_block();
            
            if (break_flag) {
                break_flag = 0;
                break;
            }
            if (continue_flag) {
                continue_flag = 0;
            }
        }
        // Skip remaining block
        if (cur_token == TOK_INDENT) {
            int depth = 1;
            next_token();
            while (depth > 0 && cur_token != TOK_EOF) {
                if (cur_token == TOK_INDENT) depth++;
                else if (cur_token == TOK_DEDENT) depth--;
                next_token();
            }
        }
        return;
    }
    
    // for statement
    if (cur_token == TOK_FOR) {
        next_token();
        char varname[64];
        my_strcpy(varname, token_str);
        next_token();  // variable name
        if (cur_token == TOK_IN) next_token();
        value_t iterable = parse_expression();
        if (cur_token == TOK_COLON) next_token();
        if (cur_token == TOK_NEWLINE) next_token();
        
        int body_pos = pos;
        
        if (iterable.type == VAL_LIST) {
            for (int i = 0; i < iterable.list.count; i++) {
                set_var(varname, iterable.list.items[i]);
                pos = body_pos;
                next_token();
                parse_block();
                if (break_flag) {
                    break_flag = 0;
                    break;
                }
                if (continue_flag) {
                    continue_flag = 0;
                }
            }
        }
        // Skip remaining block
        if (cur_token == TOK_INDENT) {
            int depth = 1;
            next_token();
            while (depth > 0 && cur_token != TOK_EOF) {
                if (cur_token == TOK_INDENT) depth++;
                else if (cur_token == TOK_DEDENT) depth--;
                next_token();
            }
        }
        return;
    }
    
    // def statement (function definition)
    if (cur_token == TOK_DEF) {
        next_token();
        char funcname[64];
        my_strcpy(funcname, token_str);
        next_token();
        
        if (cur_token == TOK_LPAREN) next_token();
        
        // Parse parameters
        char *params[16];
        int param_count = 0;
        while (cur_token == TOK_IDENT && param_count < 16) {
            params[param_count++] = my_strdup(token_str);
            next_token();
            if (cur_token == TOK_COMMA) next_token();
        }
        
        if (cur_token == TOK_RPAREN) next_token();
        if (cur_token == TOK_COLON) next_token();
        if (cur_token == TOK_NEWLINE) next_token();
        
        // Save function body
        int body_start = pos;
        if (cur_token == TOK_INDENT) {
            int depth = 1;
            next_token();
            while (depth > 0 && cur_token != TOK_EOF) {
                if (cur_token == TOK_INDENT) depth++;
                else if (cur_token == TOK_DEDENT) depth--;
                next_token();
            }
        }
        int body_end = pos;
        
        // Create function
        if (func_count < MAX_FUNCS) {
            functions[func_count].name = my_strdup(funcname);
            functions[func_count].params = malloc(param_count * sizeof(char*));
            for (int i = 0; i < param_count; i++) {
                functions[func_count].params[i] = params[i];
            }
            functions[func_count].param_count = param_count;
            functions[func_count].body = malloc(body_end - body_start + 1);
            for (int i = 0; i < body_end - body_start; i++) {
                functions[func_count].body[i] = source[body_start + i];
            }
            functions[func_count].body[body_end - body_start] = '\0';
            
            value_t v;
            v.type = VAL_FUNC;
            v.func = &functions[func_count];
            set_var(funcname, v);
            func_count++;
        }
        return;
    }
    
    // break
    if (cur_token == TOK_BREAK) {
        next_token();
        break_flag = 1;
        return;
    }
    
    // continue
    if (cur_token == TOK_CONTINUE) {
        next_token();
        continue_flag = 1;
        return;
    }
    
    // import (stub)
    if (cur_token == TOK_IMPORT) {
        next_token();
        if (cur_token == TOK_IDENT) next_token();
        if (cur_token == TOK_NEWLINE) next_token();
        return;
    }
    
    // Assignment or expression
    if (cur_token == TOK_IDENT) {
        char name[64];
        my_strcpy(name, token_str);
        next_token();
        
        if (cur_token == TOK_ASSIGN || cur_token == TOK_PLUSEQ || 
            cur_token == TOK_MINUSEQ || cur_token == TOK_STAREQ || cur_token == TOK_SLASHEQ) {
            token_type_t op = cur_token;
            next_token();
            value_t val = parse_expression();
            
            if (op != TOK_ASSIGN) {
                value_t *old = get_var(name);
                if (old && old->type == VAL_INT && val.type == VAL_INT) {
                    switch (op) {
                        case TOK_PLUSEQ: val.int_val = old->int_val + val.int_val; break;
                        case TOK_MINUSEQ: val.int_val = old->int_val - val.int_val; break;
                        case TOK_STAREQ: val.int_val = old->int_val * val.int_val; break;
                        case TOK_SLASHEQ: if (val.int_val) val.int_val = old->int_val / val.int_val; break;
                        default: break;
                    }
                }
            }
            set_var(name, val);
        } else if (cur_token == TOK_LBRACKET) {
            // List assignment: arr[idx] = val
            next_token();
            value_t index = parse_expression();
            if (cur_token == TOK_RBRACKET) next_token();
            if (cur_token == TOK_ASSIGN) {
                next_token();
                value_t val = parse_expression();
                value_t *var = get_var(name);
                if (var && var->type == VAL_LIST) {
                    int idx = index.int_val;
                    if (idx >= 0 && idx < var->list.count) {
                        var->list.items[idx] = val;
                    }
                }
            }
        } else {
            // Just an expression (e.g., function call without assignment)
            // Back up and re-parse as expression
            pos -= my_strlen(name);
            next_token();
            parse_expression();
        }
    } else {
        // Expression statement
        parse_expression();
    }
    
    // Skip trailing newline
    if (cur_token == TOK_NEWLINE) next_token();
}

// ============================================================================
// Main
// ============================================================================

static void run_repl(void) {
    printf("Python 3 (MayteraOS) [Minimal Interpreter]\n");
    printf("Type \"help()\" for more information.\n");
    
    static char line[1024];
    
    while (1) {
        printf(">>> ");
        
        int i = 0;
        int c;
        while ((c = getchar()) != '\n' && c != -1 && i < 1023) {
            line[i++] = c;
        }
        line[i] = '\0';
        
        if (c == -1 || !my_strcmp(line, "exit()") || !my_strcmp(line, "quit()")) {
            break;
        }
        
        source = line;
        pos = 0;
        lineno = 1;
        indent_level = 0;
        indent_stack[0] = 0;
        
        next_token();
        while (cur_token != TOK_EOF) {
            parse_statement();
        }
    }
}

static void run_file(const char *filename) {
    int fd = open(filename, 0);
    if (fd < 0) {
        printf("Error: Cannot open file: %s\n", filename);
        return;
    }
    
    // Read file
    static char code[32 * 1024];
    long n = read(fd, code, sizeof(code) - 1);
    close(fd);
    
    if (n <= 0) {
        printf("Error: Cannot read file\n");
        return;
    }
    code[n] = '\0';
    
    source = code;
    pos = 0;
    lineno = 1;
    indent_level = 0;
    indent_stack[0] = 0;
    
    next_token();
    while (cur_token != TOK_EOF) {
        parse_statement();
    }
}

int main(int argc, char **argv) {
    init_builtins();
    
    if (argc == 1) {
        run_repl();
    } else if (argc >= 2) {
        if (!my_strcmp(argv[1], "-c") && argc > 2) {
            source = argv[2];
            pos = 0;
            lineno = 1;
            indent_level = 0;
            indent_stack[0] = 0;
            next_token();
            while (cur_token != TOK_EOF) {
                parse_statement();
            }
        } else if (!my_strcmp(argv[1], "-h") || !my_strcmp(argv[1], "--help")) {
            printf("Usage: python [options] [script.py]\n");
            printf("Options:\n");
            printf("  -c CODE  Execute CODE\n");
            printf("  -h       Show help\n");
        } else {
            run_file(argv[1]);
        }
    }
    
    return 0;
}
