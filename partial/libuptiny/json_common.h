#ifndef LIBUPTINY_JSON_COMMON_H
#define LIBUPTINY_JSON_COMMON_H

#include <stdbool.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

// consumes a token in token_pool indexed by idx recursively, returns index immediately after the consumed token
unsigned int consume_recursive_json(unsigned int idx);

// token's length
#define JSON_TOK_LEN(token) ((token).end - (token).start)

// compare token with a string literal
bool json_str_equal(const char* json, unsigned int idx, const char* value);

#ifdef __cplusplus
}
#endif

#endif  // LIBUPTINY_JSON_COMMON_H
