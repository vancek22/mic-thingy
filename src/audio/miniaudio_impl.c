/* Single translation unit that holds the miniaudio implementation.
 * Decoding/encoding stay enabled: the offline tools read and write WAV files. */
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
