#include <stdio.h>

#include <libavcodec/avcodec.h>

#include "logger.h"
#include "raop_buffer.h"

static void
write_log(void *context, int level, const char *message)
{
	(void)context;
	(void)level;
	fprintf(stderr, "%s\n", message);
}

int
main(void)
{
	const unsigned char aes_key[16] = {0};
	const unsigned char aes_iv[16] = {0};
	const unsigned char ecdh_secret[32] = {0};
	logger_t *logger = logger_init();
	raop_buffer_t *buffer;

	if (!logger) {
		fprintf(stderr, "Could not create the test logger.\n");
		return 1;
	}
	logger_set_level(logger, LOGGER_DEBUG);
	logger_set_callback(logger, write_log, NULL);
	if (!avcodec_find_decoder(AV_CODEC_ID_H264)) {
		fprintf(stderr, "The production FFmpeg build does not contain the H.264 decoder.\n");
		logger_destroy(logger);
		return 1;
	}

	buffer = raop_buffer_init(logger, aes_key, aes_iv, ecdh_secret);
	if (!buffer) {
		fprintf(stderr, "Could not initialize the production AAC-ELD decoder.\n");
		logger_destroy(logger);
		return 1;
	}

	raop_buffer_destroy(buffer);
	logger_destroy(logger);
	puts("Validated production FFmpeg H.264 availability and AAC-ELD decoder initialization.");
	return 0;
}
