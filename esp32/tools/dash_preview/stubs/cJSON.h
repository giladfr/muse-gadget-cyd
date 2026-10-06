#pragma once
#include <string.h>
#include "cJSON_fake.h"
static inline cJSON *cJSON_Parse(const char *s){return cJSON_ParseWithLength(s, strlen(s));}
static inline int cJSON_IsNumber(const cJSON *i){return i && (i->type & 0xff)==cJSON_Number;}
#define cJSON_GetObjectItemCaseSensitive cJSON_GetObjectItem
