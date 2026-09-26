#ifndef KATZIP_PARALLEL_INTERNAL_H
#define KATZIP_PARALLEL_INTERNAL_H

#include "katzip_internal.h"

typedef struct FILE_SLOT FILE_SLOT;

typedef struct {
  FILE_SLOT *slot;
  FILE *input;
  CANDIDATE candidate;
  uint64_t memory_cost;
  int engine;
} COMPRESS_TASK;

struct FILE_SLOT {
  ENTRY *entry;
  FILE *snapshot;
  COMPRESS_TASK tasks[4];
  int task_count;
  int completed;
  int zlib_only;
};

int prepare_slot(FILE_SLOT *slot, ENTRY *entry,
  const COMPRESSION_CONFIG *config);
void finish_slot(FILE_SLOT *slot);
int compress_task(COMPRESS_TASK *task,
  const COMPRESSION_CONFIG *config);
int spool_candidate(CANDIDATE *candidate);
int write_slot(void *zip, FILE_SLOT *slot, int zip_level);

#endif
