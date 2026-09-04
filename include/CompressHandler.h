//
// Created by Intuition on 25-8-17.
//

#ifndef COMPRESSHANDLER_H
#define COMPRESSHANDLER_H

#include <stdlib.h>
#include <stdint.h>

#include "Basket.h"
#include "Error.h"

typedef struct {
    uint32_t code;
    uint8_t bits;
} HuffmanCode;

// HPACK dynamic table structure
typedef struct HuffmanNode {
    int symbol; // -1 for non-leaf, 0 ~ 256 for leaf
    struct HuffmanNode *left;
    struct HuffmanNode *right;
} HuffmanNode;

size_t hpackHuffmanEncode(const char *input, size_t inputLen, unsigned char *output);
char* hpackHuffmanDecode(const uint8_t *src, size_t srcLen);
HuffmanNode* buildHuffmanTree();
void freeHuffmanTreeFromRoot();
void writeHuffmanValue(unsigned char **ptr, const char *value, size_t valueLen);
void hpackEncodeInteger(size_t value, int prefixBits, unsigned char prefix, unsigned char **ptrPtr);
size_t hpackDecodeInteger(uint8_t *buf, size_t *pos, uint8_t prefixSize, size_t bufLen);
char* hpackDecodeString(unsigned char *payload, size_t *pos, size_t length);

typedef struct {
    size_t         decompressedPayloadSize;
    Error          error;
    unsigned char *decompressedPayload;
} DecompressedObj;

// Response body encodings; the values double as the bits of Basket.decompress.
typedef enum {
    ENCODING_IDENTITY = 0,
    ENCODING_GZIP = 1,
    ENCODING_DEFLATE = 2,
    ENCODING_BROTLI = 4,
    ENCODING_ZSTD = 8
} ContentEncoding;

// Look up "content-encoding" in `headers` and map it to an encoding
// (unknown / absent -> identity).
ContentEncoding detectContentEncoding(const ResponseHeader *headers, size_t numHeaders);

DecompressedObj* decompress_Brotli(unsigned char *payload, size_t payloadSize);
DecompressedObj* decompress_GZip(unsigned char *payload, size_t payloadSize);
DecompressedObj* decompress_Deflate(unsigned char *payload, size_t payloadSize);
DecompressedObj* decompress_Zstd(unsigned char *payload, size_t payloadSize);

// ─── incremental decoding (streaming responses) ───
// Hands one decoded piece to the consumer; returning non-zero aborts.
typedef int (*BodyConsumer)(void *ctx, const unsigned char *data, size_t len);

typedef struct StreamDecompressor StreamDecompressor;

// NULL for identity, also NULL on failure (caller reports the encoding error).
StreamDecompressor* buildStreamDecompressor(ContentEncoding encoding);

// Decode `len` bytes, emitting every decoded piece through `consume`.
// Returns 1 when the input was consumed, 0 when the consumer aborted, -1 on a
// decode error.
int feedStreamDecompressor(StreamDecompressor *decompressor, const unsigned char *data, size_t len,
                           BodyConsumer consume, void *ctx);

void freeStreamDecompressor(StreamDecompressor *decompressor);

unsigned char* base64Encode(const unsigned char *data, size_t dataLen);

#endif //COMPRESSHANDLER_H
