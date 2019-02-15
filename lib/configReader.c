#include "configReader.h"

int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
	if (tok->type == JSMN_STRING && (int) strlen(s) == tok->end - tok->start &&
			strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 0;
	}
	return -1;
}

void freeDescription(description desc) {
  puts("Cleaning up description");
  if (desc.init) {
    free(desc.uuid);
    free(desc.text);
    desc.init = 0;
    desc.uuid = NULL;
    desc.text = NULL;
  }
  puts("Done cleaning up description");
}

void freeCharacteristic(characteristic chara) {
  puts("Cleaning up characteristic");
  if (chara.init) {
    free(chara.uuid);
    for (int i = 0; i < chara.descriptionsSize; i++) {
      freeDescription(chara.descriptions[i]);
    }
    free(chara.descriptions);
    chara.init = 0;
    chara.uuid = NULL;
    chara.descriptions= NULL;
  }
  puts("Done cleaning up characteristic");
}

void freeService(service serv) {
  puts("Cleaning up service");
  if (serv.init) {
    free(serv.uuid);
    for (int i = 0; i < serv.characteristicsSize; i++) {
      freeCharacteristic(serv.characteristics[i]);
    }
    free(serv.characteristics);
    serv.init = 0;
    serv.uuid = NULL;
    serv.characteristics= NULL;
  }
  puts("Done cleaning up service");
}

description parseDescription(const char *js, jsmntok_t *t, size_t count, int *depth) {
  description d;
  int localDepth = 0;
  d.init = 0;
  
  // Check if we got an object
  if (count < 1 || t[localDepth++].type != JSMN_OBJECT) {
		fprintf(stderr, "Description object expected\n");
    //fprintf(stderr, "%.*s\n", t[localDepth-0].end-t[localDepth-0].start, js+ t[localDepth-0].start);
		return d;
	}

  // Check if the first tag is a UUID
  if (jsoneq(js, &t[localDepth++], "uuid") != 0 || t[localDepth].type != JSMN_STRING) {
    puts("uuid expected as string");
    (*depth) += localDepth;
    return d;
  }

  // Allocate memory for description uuid
  d.uuid = (char *)malloc(t[localDepth].end - t[localDepth].start + 1);

  if (d.uuid == NULL) {
    fprintf(stderr, "malloc error\n");
    (*depth) += localDepth;
    return d;
  }

  // Parse UUID from JSON text
  d.uuid = strncpy(d.uuid, js + t[localDepth].start, t[localDepth].end - t[localDepth].start);
  d.uuid[t[localDepth].end - t[localDepth].start] = '\0';

  //printf("%s\n", d.uuid);
  localDepth++;

  // Parse text from JSON text
  if (jsoneq(js, &t[localDepth++], "text") != 0 || t[localDepth].type != JSMN_STRING) {
    puts("text expected as string");
    (*depth) += localDepth;
    free(d.uuid);
    d.uuid = NULL;
    return d;
  }

  // Allocate memory for description text
  d.text= (char *)malloc(t[localDepth].end - t[localDepth].start + 1);

  if (d.text == NULL) {
    fprintf(stderr, "malloc error\n");
    (*depth) += localDepth;
    free(d.uuid);
    d.uuid = NULL;
    return d;
  }

  // Parse UUID from JSON text
  d.text = strncpy(d.text, js + t[localDepth].start, t[localDepth].end - t[localDepth].start);
  d.text[t[localDepth].end - t[localDepth].start] = '\0';

  //printf("%s\n", d.text);
  localDepth++;

  d.init=1;

  (*depth) += localDepth;
  return d;
}

characteristic parseCharacteristic(const char *js, jsmntok_t *t, size_t count, int *depth) {
  characteristic c;
  int localDepth = 0;
  int descriptionArraySize;
  c.init = 0;

  // Check if we got an object
  if (count < 1 || t[localDepth++].type != JSMN_OBJECT) {
		fprintf(stderr, "Characteristic object expected\n");
    //fprintf(stderr, "%.*s\n", t[localDepth-1].end-t[localDepth-1].start, js+ t[localDepth-1].start);
		return c;
	}

  // Check if the first tag is a UUID
  if (jsoneq(js, &t[localDepth++], "uuid") != 0 || t[localDepth].type != JSMN_STRING) {
    puts("uuid expected as string");
    (*depth) += localDepth;
    return c;
  }

  // Allocate memory for characteristic uuid
  c.uuid = (char *)malloc(t[localDepth].end - t[localDepth].start + 1);

  if (c.uuid == NULL) {
    fprintf(stderr, "malloc error\n");
    (*depth) += localDepth;
    return c;
  }

  // Parse UUID from JSON text
  c.uuid = strncpy(c.uuid, js + t[localDepth].start, t[localDepth].end - t[localDepth].start);
  c.uuid[t[localDepth].end - t[localDepth].start] = '\0';

  //printf("%s\n", c.uuid);
  localDepth++;

  // Parse descriptions from JSON text
  if (jsoneq(js, &t[localDepth++], "descriptions") != 0 || t[localDepth].type != JSMN_ARRAY) {
    puts("Description is expected as array");
    free(c.uuid);
    c.uuid = NULL;
    return c;
  }

  // Initialize descriptions array
  descriptionArraySize = t[localDepth].size;
  c.descriptionsSize = descriptionArraySize;
  c.descriptions = (description*)malloc(sizeof(description) * descriptionArraySize);

  if (c.descriptions == NULL) {
    puts("Error allocating memory for descriptions");
    free(c.uuid);
    c.uuid = NULL;
    return c;
  }

  // Go through the descriptions array
  for (int i=0; i < descriptionArraySize; i++) {
    printf("    Parsing %d. description\n", i);
    c.descriptions[i] = parseDescription(js, t+1+localDepth, count-localDepth, &localDepth);
  }

  // Check if initialization of the descritions are off, meaning our offsets are off
  for (int i=0; i < descriptionArraySize; i++) {
    if (!c.descriptions[i].init) {
      freeCharacteristic(c);
      return c;
    }
  }

  localDepth++;
  c.init=1;

  (*depth) += localDepth;
  return c;
}

service parseService(const char *js, jsmntok_t *t, size_t count, int *depth) {
  service s;
  int localDepth = 0;
  int characteristicArraySize;
  s.init = 0;

  if (count < 1 || t[localDepth++].type != JSMN_OBJECT) {
		fprintf(stderr, "Service object expected\n");
		return s;
	}

  if (jsoneq(js, &t[localDepth++], "uuid") != 0 || t[localDepth].type != JSMN_STRING) {
    puts("uuid expected as string");
    (*depth) += localDepth;
    return s;
  }
  
  // Allocate memory for service uuid
  s.uuid = (char *)malloc(t[localDepth].end - t[localDepth].start + 1);

  if (s.uuid == NULL) {
    fprintf(stderr, "malloc error\n");
    (*depth) += localDepth;
    return s;
  }

  // Parse UUID from JSON text
  s.uuid = strncpy(s.uuid, js + t[localDepth].start, t[localDepth].end - t[localDepth].start);
  s.uuid[t[localDepth].end - t[localDepth].start] = '\0';

  //printf("%s\n", s.uuid);
  localDepth++;

  // Parse characteristics from JSON text
  if (jsoneq(js, &t[localDepth++], "characteristics") != 0 || t[localDepth].type != JSMN_ARRAY) {
    puts("Characteristics is expected as array");
    free(s.uuid);
    s.uuid = NULL;
    return s;
  }

  // Initialize characteristics array
  characteristicArraySize = t[localDepth].size;
  s.characteristicsSize = characteristicArraySize;
  //printf("charSize: %d\n", s.characteristicsSize);
  s.characteristics = (characteristic*)malloc(sizeof(characteristic) * characteristicArraySize);

  if (s.characteristics == NULL) {
    puts("Error allocating memory for characteristics");
    free(s.uuid);
    s.uuid = NULL;
    return s;
  }

  // Go through the characteristics array
  for (int i=0; i < characteristicArraySize; i++) {
    printf("  Parsing %d. characteristic\n", i);
    s.characteristics[i] = parseCharacteristic(js, t+1+localDepth, count-localDepth, &localDepth);
  }

  // Check if initialization of the characteristics are off, meaning our offsets are off
  for (int i=0; i < characteristicArraySize; i++) {
    if (!s.characteristics[i].init) {
      freeService(s);
      return s;
    }
  }

  localDepth++;
  s.init=1;

  (*depth) += localDepth;
  return s;
}

service * parseConfigFile(const char *js, jsmntok_t *t, size_t count, int *serviceArraySize) {

  service *s = NULL;
  int depth = 0;

  if (count < 1 || t[depth++].type != JSMN_OBJECT) {
		fprintf(stderr, "Object expected\n");
		return NULL;
	}

  if (jsoneq(js, &t[depth++], "services") == 0) {
      printf("Services found\n");

      if ((&t[depth])->type != JSMN_ARRAY) {
        fprintf(stderr, "services should be an array\n");
        return NULL;
      }

      *serviceArraySize = (&t[depth])->size;
      //printf("size: %d\n", *serviceArraySize);
      s = (service*)malloc(sizeof(service) * *serviceArraySize);

      for (int i=0; i < *serviceArraySize; i++) {
        printf("Parsing %d. service\n", i);
        s[i] = parseService(js, t+1+depth, count-depth, &depth);
      }

      for (int i=0; i < *serviceArraySize; i++) {
        if (!s[i].init) {
          freeService(s[i]);
          return NULL;
        }
      }

  } else {
    fprintf(stderr, "services tag should be the first\n");
  }

  return s;
}

service * readConfig(const char *path, int *servicesSize) {

	int r;
  jsmn_parser p;
	jsmntok_t *tok;
	size_t tokcount = 2;
  int fd;
  off_t fileSize;
  ssize_t read_size;
  char *buffer;
  service *services;
  struct stat stbuf;

	/* Prepare parser */
	jsmn_init(&p);

	/* Allocate some tokens as a start */
	tok = malloc(sizeof(*tok) * tokcount);
	if (tok == NULL) {
		fprintf(stderr, "malloc(): errno=%d\n", errno);
		return NULL;
	}

  fd = open(path, 'r');
  if (fd == -1) {
    fprintf(stderr, "fd error\n");
    return NULL;
  }

  if ((fstat(fd, &stbuf) != 0) || (!S_ISREG(stbuf.st_mode))) {
    fprintf(stderr, "fstat/S_ISREG error\n");
    return NULL;
  }

  fileSize = stbuf.st_size;
  buffer = (char*)malloc(fileSize + 1);

  if (buffer == NULL) {
    fprintf(stderr, "malloc error\n");
    return NULL;
  }

  read_size = read(fd, buffer, sizeof(char) * fileSize);
  buffer[fileSize] = '\0';

  if (fileSize != read_size) {
    free(buffer);
    buffer = NULL;
    close(fd);
    fprintf(stderr, "read went wrong\n");
    return NULL;
  }

  close(fd);

  do {
    r = jsmn_parse(&p, buffer, fileSize, tok, tokcount);
    if (r == JSMN_ERROR_NOMEM) {
      tokcount = tokcount * 2;
      tok = realloc_it(tok, sizeof(*tok) * tokcount);
      if (tok == NULL) {
        return NULL;
      }
    }
  } while(r <= 0);
  //dump(buffer, tok, p.toknext, 0);
  services = parseConfigFile(buffer, tok, p.toknext, servicesSize);
  
  free(buffer);
  return services;
}
