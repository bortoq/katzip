#ifndef KATZIP_DEFAULTS_H
#define KATZIP_DEFAULTS_H

#include "katzip_internal.h"

#define DEFAULT_ZLIB_AFTER (64U * 1024U * 1024U)
#define FAST_LEVEL_COUNT 6
#define ECT_FIRST_LEVEL 7
#define HIGHEST_LEVEL 9

/* Presets are the only source for both compression and generated INI files. */
static const int fast_defaults[] = {1, 2, 3, 5, 6, 8};

/* Levels 7 and 8 are tuned; level 9 uses ECT mode 9 defaults. */
static const ZopfliOptions ect_defaults[] = {
  {
    .numiterations = 3,
    .filter_style = 1,
    .skipdynamic = 80,
    .trystatic = 0,
    .noblocksplit = 1000,
    .noblocksplitlz = 200,
    .num = 9,
    .searchext = 1,
    .reuse_costmodel = 1,
    .useCache = 1,
    .multithreading = 0,
    .isPNG = 0,
    .replaceCodes = 1001,
    .twice = 1,
    .ultra = 1,
    .greed = 48,
    .entropysplit = 1,
    .advanced = 1
  },
  {
    .numiterations = 4,
    .filter_style = 1,
    .skipdynamic = 80,
    .trystatic = 300,
    .noblocksplit = 1000,
    .noblocksplitlz = 2250,
    .num = 9,
    .searchext = 1,
    .reuse_costmodel = 1,
    .useCache = 1,
    .multithreading = 0,
    .isPNG = 0,
    .replaceCodes = 1001,
    .twice = 1,
    .ultra = 1,
    .greed = 48,
    .entropysplit = 1,
    .advanced = 1
  },
  {
    .numiterations = 60,
    .filter_style = 3,
    .skipdynamic = 80,
    .trystatic = 3000,
    .noblocksplit = 800,
    .noblocksplitlz = 100,
    .num = 9,
    .searchext = 2,
    .reuse_costmodel = 1,
    .useCache = 1,
    .multithreading = 0,
    .isPNG = 0,
    .replaceCodes = 1001,
    .twice = 0,
    .ultra = 1,
    .greed = 258,
    .entropysplit = 0,
    .advanced = 1
  }
};

static const turtledeflate_config_t turtle_defaults[] = {
  {
    .i_compression_level = 9,
    .i_maximum_block_size = 1000000,
    .i_maximum_subblocks = 8,
    .i_max_block_splitter_iterations = 1,
    .i_max_internal_block_splitter_iterations = 8,
    .i_block_splitter_num_points = 3,
    .i_block_splitter_center_dist = 1,
    .i_block_splitter_min_range_for_points = 256,
    .b_block_splitter_push_split = false,
    .i_min_start_fp = -2,
    .i_max_start_fp = 2,
    .i_num_start_fp = 2,
    .i_verbose = 0
  }
};

#endif
