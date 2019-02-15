#include <lib/jsmn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  int init;
  char *uuid;
  char *text;
} description;

typedef struct {
  int init;
  char *uuid;
  int descriptionsSize;
  description *descriptions;
} characteristic;


typedef struct {
  int init;
  char *uuid;
  int characteristicsSize;
  characteristic *characteristics;
} service;

/* Function realloc_it() is a wrapper function for standard realloc()
 * with one difference - it frees old memory pointer in case of realloc
 * failure. Thus, DO NOT use old data pointer in anyway after call to
 * realloc_it(). If your code has some kind of fallback algorithm if
 * memory can't be re-allocated - use standard realloc() instead.
 */
inline void *realloc_it(void *ptrmem, size_t size) {
	void *p = realloc(ptrmem, size);
	if (!p)  {
		free (ptrmem);
		fprintf(stderr, "realloc(): errno=%d\n", errno);
	}
	return p;
}

/*
 * Compares a string with a json element
 */
int jsoneq(const char *json, jsmntok_t *tok, const char *s);

/*
 * Free the allocated description struct
 */
void freeDescription(description desc);

/*
 * Free the allocated characteristic struct
 */
void freeCharacteristic(characteristic chara);

/*
 * Free the allocated service struct
 */
void freeService(service serv);

/*
 * Parse a json element representing a description struct
 */
description parseDescription(const char *js, jsmntok_t *t, size_t count, int *depth);

/*
 * Parse a json element representing a characteristic struct
 */
characteristic parseCharacteristic(const char *js, jsmntok_t *t, size_t count, int *depth);

/*
 * Parse a json element representing a service struct
 */
service parseService(const char *js, jsmntok_t *t, size_t count, int *depth);

/*
 * Parse a json element representing the configuration file
 */
service * parseConfigFile(const char *js, jsmntok_t *t, size_t count, int *serviceArraySize);

service * readConfig(const char *path, int *servicesSize);
