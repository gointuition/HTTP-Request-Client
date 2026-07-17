//
// Created by Intuition on 25-8-17.
//

#ifndef COMPRESSHANDLER_H
#define COMPRESSHANDLER_H

#include <stdlib.h>

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

DecompressedObj* decompress_Brotli(unsigned char *payload, size_t payloadSize);
DecompressedObj* decompress_GZip(unsigned char *payload, size_t payloadSize);
DecompressedObj* decompress_Deflate(unsigned char *payload, size_t payloadSize);
DecompressedObj* decompress_Zstd(unsigned char *payload, size_t payloadSize);

unsigned char* base64Encode(const unsigned char *data, size_t dataLen);

#endif //COMPRESSHANDLER_H
