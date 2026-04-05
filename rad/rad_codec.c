/*
 * rad_codec.c -- Codec registry
 */

#include "rad.h"
#include <string.h>

static const rad_codec_ops_t *codecs[] = {
	&rad_codec_pcmu,
	&rad_codec_pcma,
	&rad_codec_l16,
#ifdef RAPTOR_AAC
	&rad_codec_aac,
#endif
#ifdef RAPTOR_OPUS
	&rad_codec_opus,
#endif
	NULL,
};

const rad_codec_ops_t *rad_codec_find(const char *name)
{
	if (!name)
		return NULL;
	for (int i = 0; codecs[i]; i++) {
		if (strcasecmp(name, codecs[i]->name) == 0)
			return codecs[i];
	}
	return NULL;
}
