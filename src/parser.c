#include "arkin_core.h"
#include "arkin_log.h"
#include "internal.h"

typedef struct FileParser FileParser;
struct FileParser {
    FileParser *next;

    ArStr source;
    U32 i;
    U32 token_start;
    U32 token_end;
    U32 last_token_end;
};

U8 file_parser_peek(FileParser parser) {
    return parser.source.data[parser.i];
}

U8 file_parser_peek_next(FileParser parser) {
    return parser.source.data[parser.i + 1];
}

typedef enum {
    MODULE_NONE,
    MODULE_MODULE,
    MODULE_VERT,
    MODULE_FRAG,
} ModuleType;

typedef struct Module Module;
struct Module {
    ArStr code;
    ModuleType type;
};

typedef struct Parser Parser;
struct Parser {
    ArArena *arena;
    FileParser *file_parser_stack;
    ModuleType current_module;
    ArStrList module_parts;
    ArHashMap *module_map;
    ArHashMap *ctype_map;
    ArStr module_name;
    struct {
        ArStr name;
        Module vert;
        Module frag;
    } program;
};

const ArStr GLSL_KEYWORDS[] = {
    ar_str_lit("define"),
    ar_str_lit("undef"),
    ar_str_lit("if"),
    ar_str_lit("ifdef"),
    ar_str_lit("ifndef"),
    ar_str_lit("else"),
    ar_str_lit("elif"),
    ar_str_lit("endif"),
    ar_str_lit("error"),
    ar_str_lit("pragma"),
    ar_str_lit("extension"),
    ar_str_lit("version"),
    ar_str_lit("line"),
};

typedef enum {
    TOKEN_END,
    TOKEN_MODULE,
    TOKEN_VERT,
    TOKEN_FRAG,
    TOKEN_PROGRAM,
    TOKEN_INCLUDE,
    TOKEN_INCLUDE_MODULE,
    TOKEN_CTYPEDEF,

    TOKEN_ERROR,
    TOKEN_GLSL,
} TokenType;

const ArStr KEYWORDS[] = {
    ar_str_lit("end"),
    ar_str_lit("module"),
    ar_str_lit("vert"),
    ar_str_lit("frag"),
    ar_str_lit("program"),
    ar_str_lit("include"),
    ar_str_lit("include_module"),
    ar_str_lit("ctypedef"),
};

const U32 KEYWORD_ARG_COUNT[] = {
    0,
    1,
    1,
    1,
    3,
    1,
    1,
    2,
};

typedef struct Token Token;
struct Token {
    TokenType type;
    ArStr error;
    ArStr args[4];
};

ArStr extract_statement(FileParser *parser) {
    U32 start = parser->i;
    while (file_parser_peek(*parser) != '\n') {
        parser->i++;
    }
    U32 end = parser->i - 1;
    return ar_str_sub(parser->source, start, end);
}

ArStrList split_statement(ArArena *arena, ArStr statement) {
    ArStrList list = {0};

    U32 i = 0;
    while (i < statement.len) {
        while (ar_char_is_whitespace(statement.data[i])) {
            i++;
        }

        U32 start = i;
        while (!ar_char_is_whitespace(statement.data[i])) {
            i++;
        }
        U32 end = i - 1;
        ArStr word = ar_str_sub(statement, start, end);
        ar_str_list_push(arena, &list, word);
    }

    return list;
}

TokenType match_token_type(ArStr keyword) {
    for (U32 i = 0; i < ar_arrlen(KEYWORDS); i++) {
        if (ar_str_match(keyword, KEYWORDS[i], AR_STR_MATCH_FLAG_EXACT)) {
            return i;
        }
    }

    for (U32 i = 0; i < ar_arrlen(GLSL_KEYWORDS); i++) {
        if (ar_str_match(keyword, GLSL_KEYWORDS[i], AR_STR_MATCH_FLAG_EXACT)) {
            return TOKEN_GLSL;
        }
    }

    return TOKEN_ERROR;
}

Token tokenize_statement_list(ArArena *err_arena, ArStrList statement_list) {
    Token token = {0};

    ArStrListNode *curr = statement_list.first;
    ArStr keyword = curr->str;
    token.type = match_token_type(keyword);

    if (token.type == TOKEN_GLSL) {
        return token;
    } else if (token.type == TOKEN_ERROR) {
        token.error = ar_str_pushf(err_arena, "%.*s: Invalid token.", (I32) keyword.len, keyword.data);
        return token;
    }

    U32 arg_count = 0;
    for (ArStrListNode *c = curr->next; c != NULL; c = c->next) {
        arg_count++;
    }

    if (arg_count != KEYWORD_ARG_COUNT[token.type]) {
        token.error = ar_str_pushf(err_arena, "%.*s: Expected %u argument(s), got %u.", (I32) keyword.len, keyword.data, KEYWORD_ARG_COUNT[token.type], arg_count);
        token.type = TOKEN_ERROR;
        return token;
    }

    arg_count = 0;
    for (curr = curr->next; curr != NULL; curr = curr->next) {
        token.args[arg_count] = curr->str;
        arg_count++;
    }

    return token;
}

void add_module_part(Parser *parser) {
    FileParser *file_parser = parser->file_parser_stack;
    if (file_parser->token_start - file_parser->last_token_end == 2) {
        return;
    }
    ArStr module_part = ar_str_sub(file_parser->source, file_parser->last_token_end, file_parser->token_start - 1);
    module_part = ar_str_push_copy(parser->arena, module_part);
    ar_str_list_push(parser->arena, &parser->module_parts, module_part);
}

void parse(Parser *parser, ArStr source, ArStrList paths);

void expand_token(Parser *parser, Token token, ArStrList paths) {
    switch (token.type) {
        case TOKEN_END:
            if (parser->current_module == MODULE_NONE) {
                ar_error("Extranious end statment.");
                break;
            }

            add_module_part(parser);

            Module module = {
                .code = ar_str_trim(ar_str_list_join(parser->arena, parser->module_parts)),
                .type = parser->current_module,
            };
            B8 unique = ar_hash_map_insert(parser->module_map, parser->module_name, module);
            if (!unique) {
                ar_error("%.*s: Module has already been defined.", (I32) parser->module_name.len, parser->module_name.data);
            }

            parser->current_module = MODULE_NONE;
            parser->module_name = (ArStr) {0};
            parser->module_parts = AR_STR_LIST_INIT;

            break;
        case TOKEN_MODULE:
            if (parser->current_module != MODULE_NONE) {
                ar_error("%.*s: New module started before ending the last module.", (I32) token.args[0].len, token.args[0].data);
                break;
            }

            parser->module_name = token.args[0];
            parser->current_module = MODULE_MODULE;
            break;
        case TOKEN_VERT:
            if (parser->current_module != MODULE_NONE) {
                ar_error("%.*s: New vertex module started before ending the last module.", (I32) token.args[0].len, token.args[0].data);
                break;
            }

            parser->module_name = token.args[0];
            parser->current_module = MODULE_VERT;
            break;
        case TOKEN_FRAG:
            if (parser->current_module != MODULE_NONE) {
                ar_error("%.*s: New fragment module started before ending the last module.", (I32) token.args[0].len, token.args[0].data);
                break;
            }

            parser->module_name = token.args[0];
            parser->current_module = MODULE_FRAG;
            break;
        case TOKEN_PROGRAM: {
            ArStr name = token.args[0];
            ArStr vert_module_key = token.args[1];
            ArStr frag_module_key = token.args[2];

            if (parser->program.name.data != NULL) {
                ar_error("%.*s: Program has already been defined.", (I32) name.len, name.data);
                break;
            }

            Module vert_module = ar_hash_map_get(parser->module_map, vert_module_key, Module);
            Module frag_module = ar_hash_map_get(parser->module_map, frag_module_key, Module);

            B8 failed = false;
            if (vert_module.type != MODULE_VERT) {
                ar_error("%.*s: Vertex module not found.", (I32) vert_module_key.len, vert_module_key.data);
                failed = true;
            }
            if (frag_module.type != MODULE_FRAG) {
                ar_error("%.*s: Fragment module not found.", (I32) frag_module_key.len, frag_module_key.data);
                failed = true;
            }
            if (failed) {
                break;
            }

            parser->program.name = name;
            parser->program.vert = vert_module;
            parser->program.frag = frag_module;
        } break;
        case TOKEN_INCLUDE:
            if (paths.first == NULL) {
                ar_error("Cannot include files without providing search paths.");
                break;
            }

            ArTemp scratch = ar_scratch_get(&parser->arena, 1);
            FILE *fp = NULL;
            ArStr path = {0};
            for (ArStrListNode *curr = paths.first; curr != NULL; curr = curr->next) {
                ArStr include_path = ar_str_pushf(scratch.arena, "%.*s/%.*s", (I32) curr->str.len, curr->str.data, (I32) token.args[0].len, token.args[0].data);
                const char *cstr_include_path = ar_str_to_cstr(scratch.arena, include_path);
                fp = fopen(cstr_include_path, "rb");
                if (fp != NULL) {
                    path = include_path;
                    break;
                }
            }

            if (fp == NULL) {
                ar_error("Couldn't find file %.*s, in the provided paths.", (I32) token.args[0].len, token.args[0].data);
            } else {
                fclose(fp);
            }

            ArStr imported_file = read_file(scratch.arena, path);
            ArStr path_dir = dirname(path);
            ar_str_list_push_front(scratch.arena, &paths, path_dir);
            ar_str_list_pop(&paths);
            parse(parser, imported_file, paths);

            ar_scratch_release(&scratch);

            break;
        case TOKEN_INCLUDE_MODULE: {
            ArStr module_value = ar_hash_map_get(parser->module_map, token.args[0], ArStr);
            if (module_value.data == NULL) {
                ar_error("%.*s: Module couldn't be found.", (I32) token.args[0].len, token.args[0].data);
                break;
            }
            ar_str_list_push(parser->arena, &parser->module_parts, module_value);
        } break;
        case TOKEN_CTYPEDEF: {
            ArArena *hm_arena = ar_hash_map_get_arena(parser->ctype_map);
            ArStr glsl_type = ar_str_push_copy(hm_arena, token.args[0]);
            ArStr ctype = ar_str_push_copy(hm_arena, token.args[1]);
            ar_hash_map_insert(parser->ctype_map, glsl_type, ctype);
        } break;

        case TOKEN_ERROR:
            ar_error("%.*s", (I32) token.error.len, token.error.data);
            break;
        case TOKEN_GLSL:
            break;
    }

    if (parser->current_module != MODULE_NONE) {
        add_module_part(parser);
    }
}

void parse(Parser *parser, ArStr source, ArStrList paths) {
    FileParser file_parser = {
        .source = source,
    };

    ar_sll_stack_push(parser->file_parser_stack, &file_parser);

    while (file_parser.i < file_parser.source.len) {
        if (file_parser_peek(file_parser) == '/' && file_parser_peek_next(file_parser) == '/') {
            while (file_parser_peek(file_parser) != '\n') {
                file_parser.i++;
            }
            continue;
        }

        if (file_parser_peek(file_parser) == '#') {
            file_parser.last_token_end = file_parser.token_end;
            file_parser.token_start = file_parser.i;
            file_parser.i++;
            ArStr statement = extract_statement(&file_parser);
            ArTemp scratch = ar_scratch_get(&parser->arena, 1);
            ArStrList statement_list = split_statement(scratch.arena, statement);
            Token token = tokenize_statement_list(scratch.arena, statement_list);
            expand_token(parser, token, paths);
            file_parser.token_end = file_parser.i;

            if (token.type == TOKEN_GLSL) {
                FileParser *file_parser = parser->file_parser_stack;
                ArStr module_part = ar_str_sub(file_parser->source, file_parser->token_start, file_parser->token_end);
                module_part = ar_str_push_copy(parser->arena, module_part);
                ar_str_list_push(parser->arena, &parser->module_parts, module_part);
            }
            ar_scratch_release(&scratch);
        }


        file_parser.i++;
    }

    ar_sll_stack_pop(parser->file_parser_stack);
}

static U64 hash_str(const void *key, U64 len) {
    (void) len;
    const ArStr *_key = key;
    return ar_fvn1a_hash(_key->data, _key->len);
}

static B8 str_eq(const void *a, const void *b, U64 len) {
    (void) len;
    const ArStr *_a = a;
    const ArStr *_b = b;
    return ar_str_match(*_a, *_b, AR_STR_MATCH_FLAG_EXACT);
}

ParsedShader parse_shader(ArArena *arena, ArStr source, ArStrList paths) {
    ArTemp scratch = ar_scratch_get(&arena, 1);

    ArHashMapDesc module_map_desc = {
        .arena = scratch.arena,
        .capacity = 32,

        .hash_func = hash_str,
        .eq_func = str_eq,

        .key_size = sizeof(ArStr),
        .value_size = sizeof(Module),
        .null_value = &(Module) {0},
    };

    ArHashMapDesc ctype_map_desc = {
        .arena = arena,
        .capacity = 32,

        .hash_func = hash_str,
        .eq_func = str_eq,

        .key_size = sizeof(ArStr),
        .value_size = sizeof(ArStr),
        .null_value = &(ArStr) {0},
    };

    Parser parser = {
        .arena = scratch.arena,
        .module_map = ar_hash_map_init(module_map_desc),
        .ctype_map = ar_hash_map_init(ctype_map_desc),
    };

    parse(&parser, source, paths);

    ParsedShader shader = {
        .program = {
            .name = ar_str_push_copy(arena, parser.program.name),
            .vertex_source = ar_str_push_copy(arena, parser.program.vert.code),
            .fragment_source = ar_str_push_copy(arena, parser.program.frag.code),
        },
        .ctypes = parser.ctype_map,
    };

    ar_scratch_release(&scratch);

    return shader;
}
