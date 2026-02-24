#include "jsmn.h"

static jsmntok_t *jsmn_alloc(jsmn_parser *parser, jsmntok_t *tokens, size_t num_tokens) {
    if (parser->toknext >= num_tokens) {
        return NULL;
    }
    jsmntok_t *tok = &tokens[parser->toknext++];
    tok->start = -1;
    tok->end = -1;
    tok->size = 0;
    tok->parent = -1;
    tok->type = JSMN_UNDEFINED;
    return tok;
}

static void jsmn_fill(jsmntok_t *token, jsmntype_t type, int start, int end) {
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

void jsmn_init(jsmn_parser *parser) {
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t num_tokens) {
    int start = (int)parser->pos;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '\t' || c == '\r' || c == '\n' || c == ' ' || c == ',' || c == ']' || c == '}') {
            break;
        }
        if (c < 32 || c >= 127) {
            return JSMN_ERROR_INVAL;
        }
    }

    jsmntok_t *tok = jsmn_alloc(parser, tokens, num_tokens);
    if (!tok) {
        parser->pos = (unsigned int)start;
        return JSMN_ERROR_NOMEM;
    }

    jsmn_fill(tok, JSMN_PRIMITIVE, start, (int)parser->pos);
    tok->parent = parser->toksuper;
    parser->pos--;
    return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t num_tokens) {
    int start = (int)parser->pos;

    parser->pos++;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '"') {
            jsmntok_t *tok = jsmn_alloc(parser, tokens, num_tokens);
            if (!tok) {
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_NOMEM;
            }
            jsmn_fill(tok, JSMN_STRING, start + 1, (int)parser->pos);
            tok->parent = parser->toksuper;
            return 0;
        }

        if (c == '\\' && parser->pos + 1 < len) {
            parser->pos++;
            switch (js[parser->pos]) {
            case '"':
            case '/':
            case '\\':
            case 'b':
            case 'f':
            case 'r':
            case 'n':
            case 't':
                break;
            case 'u':
                parser->pos += 4;
                break;
            default:
                return JSMN_ERROR_INVAL;
            }
        }
    }

    parser->pos = (unsigned int)start;
    return JSMN_ERROR_PART;
}

int jsmn_parse(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, unsigned int num_tokens) {
    int r;
    int count = (int)parser->toknext;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        jsmntok_t *tok;

        switch (c) {
        case '{':
        case '[':
            count++;
            tok = jsmn_alloc(parser, tokens, num_tokens);
            if (!tok) {
                return JSMN_ERROR_NOMEM;
            }
            if (parser->toksuper != -1) {
                tokens[parser->toksuper].size++;
                tok->parent = parser->toksuper;
            }
            tok->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
            tok->start = (int)parser->pos;
            parser->toksuper = (int)parser->toknext - 1;
            break;

        case '}':
        case ']':
            if (parser->toknext < 1) {
                return JSMN_ERROR_INVAL;
            }
            for (int i = (int)parser->toknext - 1; i >= 0; i--) {
                tok = &tokens[i];
                if (tok->start != -1 && tok->end == -1) {
                    if ((tok->type == JSMN_OBJECT && c == ']') ||
                        (tok->type == JSMN_ARRAY && c == '}')) {
                        return JSMN_ERROR_INVAL;
                    }
                    tok->end = (int)parser->pos + 1;
                    parser->toksuper = tok->parent;
                    break;
                }
            }
            break;

        case '"':
            r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
            if (r < 0) {
                return r;
            }
            count++;
            if (parser->toksuper != -1) {
                tokens[parser->toksuper].size++;
            }
            break;

        case '\t':
        case '\r':
        case '\n':
        case ' ':
        case ':':
        case ',':
            break;

        default:
            r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
            if (r < 0) {
                return r;
            }
            count++;
            if (parser->toksuper != -1) {
                tokens[parser->toksuper].size++;
            }
            break;
        }
    }

    for (unsigned int i = parser->toknext; i > 0; i--) {
        if (tokens[i - 1].start != -1 && tokens[i - 1].end == -1) {
            return JSMN_ERROR_PART;
        }
    }

    return count;
}
