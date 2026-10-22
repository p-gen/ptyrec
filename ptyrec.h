#ifndef PTYREC_H
#define PTYREC_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#define CAP 65536U
#define ARG_CAP 4096U
#define DEPTH 256U
#define GRACE_MS 2000U
#define DRAIN_MS 2000U

/* Live queue bytes occupy [first,last); no text terminator is stored. */
/* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
struct queue
{
  unsigned char data[CAP];
  size_t        first;
  size_t        last;
};

/* The stack owns streams; queued records the current unit length. */
/* sent/delivered prevent duplicate cues after an interactive-input drain. */
/* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""
 */
struct script
{
  FILE         *files[DEPTH];
  size_t        depth;
  unsigned      meta;
  unsigned      control;
  unsigned long delay;
  uint64_t      due;
  int           pending;
  int           sent;
  int           delivered;
  unsigned char bytes[ARG_CAP + 3U];
  size_t        length;
  size_t        position;
  size_t        unit;
  size_t        queued;
};

/* Map keys/text are binary and use explicit lengths; later nodes win. */
/* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
struct subtitle_map
{
  struct subtitle_map *next;

  /* length must be nonzero; text_length may be zero to suppress text. */
  /* ''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''' */
  size_t length;
  size_t text_length;

  unsigned char key[ARG_CAP];
  unsigned char text[ARG_CAP];
};

#endif
