//
// Created by Intuition on 25-8-17.
//

#include "CompressHandler.h"

#include <string.h>
#include <zlib.h>
#include <stdio.h>

#include "Log.h"

#include "brotli/decode.h"
#include "openssl/base64.h"
#include "zstd.h"

// HTTP/2 Huffman code table (RFC 7541)
static const HuffmanCode huffmanTable[] = {
    {0x1ff8, 13}, {0x7fffd8, 23}, {0xfffffe2, 28}, {0xfffffe3, 28}, {0xfffffe4, 28}, {0xfffffe5, 28},
    {0xfffffe6, 28}, {0xfffffe7, 28}, {0xfffffe8, 28}, {0xffffea, 24}, {0x3ffffffc, 30}, {0xfffffe9, 28},
    {0xfffffea, 28}, {0x3ffffffd, 30}, {0xfffffeb, 28}, {0xfffffec, 28}, {0xfffffed, 28}, {0xfffffee, 28},
    {0xfffffef, 28}, {0xffffff0, 28}, {0xffffff1, 28}, {0xffffff2, 28}, {0x3ffffffe, 30}, {0xffffff3, 28},
    {0xffffff4, 28}, {0xffffff5, 28}, {0xffffff6, 28}, {0xffffff7, 28}, {0xffffff8, 28}, {0xffffff9, 28},
    {0xffffffa, 28}, {0xffffffb, 28}, {0x14, 6}, {0x3f8, 10}, {0x3f9, 10}, {0xffa, 12}, {0x1ff9, 13},
    {0x15, 6}, {0xf8, 8}, {0x7fa, 11}, {0x3fa, 10}, {0x3fb, 10}, {0xf9, 8}, {0x7fb, 11}, {0xfa, 8},
    {0x16, 6}, {0x17, 6}, {0x18, 6}, {0x0, 5}, {0x1, 5}, {0x2, 5}, {0x19, 6}, {0x1a, 6}, {0x1b, 6},
    {0x1c, 6}, {0x1d, 6}, {0x1e, 6}, {0x1f, 6}, {0x5c, 7}, {0xfb, 8}, {0x7ffc, 15}, {0x20, 6}, {0xffb, 12},
    {0x3fc, 10}, {0x1ffa, 13}, {0x21, 6}, {0x5d, 7}, {0x5e, 7}, {0x5f, 7}, {0x60, 7}, {0x61, 7}, {0x62, 7},
    {0x63, 7}, {0x64, 7}, {0x65, 7}, {0x66, 7}, {0x67, 7}, {0x68, 7}, {0x69, 7}, {0x6a, 7}, {0x6b, 7},
    {0x6c, 7}, {0x6d, 7}, {0x6e, 7}, {0x6f, 7}, {0x70, 7}, {0x71, 7}, {0x72, 7}, {0xfc, 8}, {0x73, 7},
    {0xfd, 8}, {0x1ffb, 13}, {0x7fff0, 19}, {0x1ffc, 13}, {0x3ffc, 14}, {0x22, 6}, {0x7ffd, 15}, {0x3, 5},
    {0x23, 6}, {0x4, 5}, {0x24, 6}, {0x5, 5}, {0x25, 6}, {0x26, 6}, {0x27, 6}, {0x6, 5}, {0x74, 7},
    {0x75, 7}, {0x28, 6}, {0x29, 6}, {0x2a, 6}, {0x7, 5}, {0x2b, 6}, {0x76, 7}, {0x2c, 6}, {0x8, 5},
    {0x9, 5}, {0x2d, 6}, {0x77, 7}, {0x78, 7}, {0x79, 7}, {0x7a, 7}, {0x7b, 7}, {0x7ffe, 15}, {0x7fc, 11},
    {0x3ffd, 14}, {0x1ffd, 13}, {0xffffffc, 28}, {0xfffe6, 20}, {0x3fffd2, 22}, {0xfffe7, 20}, {0xfffe8, 20},
    {0x3fffd3, 22}, {0x3fffd4, 22}, {0x3fffd5, 22}, {0x7fffd9, 23}, {0x3fffd6, 22}, {0x7fffda, 23},
    {0x7fffdb, 23}, {0x7fffdc, 23}, {0x7fffdd, 23}, {0x7fffde, 23}, {0xffffeb, 24}, {0x7fffdf, 23},
    {0xffffec, 24}, {0xffffed, 24}, {0x3fffd7, 22}, {0x7fffe0, 23}, {0xffffee, 24}, {0x7fffe1, 23},
    {0x7fffe2, 23}, {0x7fffe3, 23}, {0x7fffe4, 23}, {0x1fffdc, 21}, {0x3fffd8, 22}, {0x7fffe5, 23},
    {0x3fffd9, 22}, {0x7fffe6, 23}, {0x7fffe7, 23}, {0xffffef, 24}, {0x3fffda, 22}, {0x1fffdd, 21},
    {0xfffe9, 20}, {0x3fffdb, 22}, {0x3fffdc, 22}, {0x7fffe8, 23}, {0x7fffe9, 23}, {0x1fffde, 21},
    {0x7fffea, 23}, {0x3fffdd, 22}, {0x3fffde, 22}, {0xfffff0, 24}, {0x1fffdf, 21}, {0x3fffdf, 22},
    {0x7fffeb, 23}, {0x7fffec, 23}, {0x1fffe0, 21}, {0x1fffe1, 21}, {0x3fffe0, 22}, {0x1fffe2, 21},
    {0x7fffed, 23}, {0x3fffe1, 22}, {0x7fffee, 23}, {0x7fffef, 23}, {0xfffea, 20}, {0x3fffe2, 22},
    {0x3fffe3, 22}, {0x3fffe4, 22}, {0x7ffff0, 23}, {0x3fffe5, 22}, {0x3fffe6, 22}, {0x7ffff1, 23},
    {0x3ffffe0, 26}, {0x3ffffe1, 26}, {0xfffeb, 20}, {0x7fff1, 19}, {0x3fffe7, 22}, {0x7ffff2, 23},
    {0x3fffe8, 22}, {0x1ffffec, 25}, {0x3ffffe2, 26}, {0x3ffffe3, 26}, {0x3ffffe4, 26}, {0x7ffffde, 27},
    {0x7ffffdf, 27}, {0x3ffffe5, 26}, {0xfffff1, 24}, {0x1ffffed, 25}, {0x7fff2, 19}, {0x1fffe3, 21},
    {0x3ffffe6, 26}, {0x7ffffe0, 27}, {0x7ffffe1, 27}, {0x3ffffe7, 26}, {0x7ffffe2, 27}, {0xfffff2, 24},
    {0x1fffe4, 21}, {0x1fffe5, 21}, {0x3ffffe8, 26}, {0x3ffffe9, 26}, {0xffffffd, 28}, {0x7ffffe3, 27},
    {0x7ffffe4, 27}, {0x7ffffe5, 27}, {0xfffec, 20}, {0xfffff3, 24}, {0xfffed, 20}, {0x1fffe6, 21},
    {0x3fffe9, 22}, {0x1fffe7, 21}, {0x1fffe8, 21}, {0x7ffff3, 23}, {0x3fffea, 22}, {0x3fffeb, 22},
    {0x1ffffee, 25}, {0x1ffffef, 25}, {0xfffff4, 24}, {0xfffff5, 24}, {0x3ffffea, 26}, {0x7ffff4, 23},
    {0x3ffffeb, 26}, {0x7ffffe6, 27}, {0x3ffffec, 26}, {0x3ffffed, 26}, {0x7ffffe7, 27}, {0x7ffffe8, 27},
    {0x1fffff0, 25}, {0x1fffff1, 25}, {0x3ffffee, 26}, {0x1fffff2, 25}, {0x7ffffe9, 27}, {0x1fffff3, 25},
    {0x3ffffef, 26}, {0x1fffff4, 25}, {0x1fffff5, 25}, {0x3fffff0, 26}, {0x1fffff6, 25}, {0x3fffff1, 26},
    {0x3fffff2, 26}, {0x7ffffea, 27}, {0x3fffff3, 26}, {0x3fffff4, 26}, {0x7ffffeb, 27}, {0x7ffffec, 27},
    {0x1fffff7, 25}, {0x3fffff5, 26}, {0x3fffff6, 26}, {0x7ffffed, 27}, {0x7ffffee, 27}, {0x1fffff8, 25},
    {0x1fffff9, 25}, {0x3fffff7, 26}, {0x3fffff8, 26}, {0x3fffff9, 26}, {0x7ffffef, 27}, {0x3fffffa, 26},
    {0x3fffffb, 26}, {0x7fffff0, 27}, {0x3fffffc, 26}, {0x3fffffd, 26}, {0x7fffff1, 27}, {0x3fffffe, 26},
    {0x7fffff2, 27}, {0x3ffffff, 26}, {0x7fffff3, 27}, {0x7fffff4, 27}, {0x7fffff5, 27}, {0xfffee, 20},
    {0x7fffff6, 27}, {0x3fffffff, 28}
};

// static HuffmanNode *huffmanRoot = NULL;
static HuffmanNode HUFFMAN_NODES[513];
HuffmanNode *huffmanRoot = NULL;

static HuffmanNode* getHuffmanNode(int index);
static char* huffmanDecodeTree(const uint8_t *src, size_t srcLen, HuffmanNode *root);

size_t hpackHuffmanEncode(const char *input, size_t inputLen, unsigned char *output) {
    size_t outputLen = 0;
    uint32_t currentByte = 0;
    uint8_t bitsUsed = 0;

    for (size_t i = 0; i < inputLen; i++) {
        unsigned char c = input[i];
        const HuffmanCode *code = &huffmanTable[c];

        // << free space for new code
        // |  add the code to the current byte
        currentByte = (currentByte << code -> bits) | code -> code;
        bitsUsed += code -> bits;

        // write complete bytes to output
        while (bitsUsed >= 8) {
            // write the first 8 bits into output
            output[outputLen++] = (currentByte >> (bitsUsed - 8)) & 0xFF;
            bitsUsed -= 8;
            // keep the remaining bits
            currentByte &= (1 << bitsUsed) - 1;
        }
    }

    // add EOS (End of String) padding if needed
    if (bitsUsed > 0) {
        // << free space for EOS code
        // | add EOS code
        currentByte = (currentByte << (8 - bitsUsed)) | ((1 << (8 - bitsUsed)) - 1);
        output[outputLen++] = currentByte & 0xFF;
    }

    return outputLen;
}

char* hpackHuffmanDecode(const uint8_t *src, size_t srcLen) {
    // if (!huffmanRoot) {
    //     huffmanRoot = buildHuffmanTree();
    // }
    return huffmanDecodeTree(src, srcLen, huffmanRoot);
}

HuffmanNode* buildHuffmanTree() {
    int index = 0;
    huffmanRoot = getHuffmanNode(index++);
    if (!huffmanRoot) return NULL;
    huffmanRoot -> symbol = -1;
    for (int i = 0; i < 257; i++) {
        uint32_t code = huffmanTable[i].code;
        uint8_t bits = huffmanTable[i].bits;
        HuffmanNode *node = huffmanRoot;
        for (int b = bits - 1; b >= 0 ; b--) {
            // TODO investigate
            int bit = (code >> b) & 1;
            if (bit == 0) {
                if (!node -> left) {
                    node -> left = getHuffmanNode(index++);
                    if (!node -> left) return NULL;
                    node -> left -> symbol = -1;
                }
                node = node -> left;
            } else {
                if (!node -> right) {
                    node -> right = getHuffmanNode(index++);
                    if (!node -> right) return NULL;
                    node -> right -> symbol = -1;
                }
                node = node -> right;
            }
        }
        node -> symbol = i;
    }
    return huffmanRoot;

    // // TODO free memory
    // HuffmanNode *root = calloc(1, sizeof(HuffmanNode));
    // root -> symbol = -1;
    // for (int i = 0; i < 257; i++) {
    //     uint32_t code = huffmanTable[i].code;
    //     uint8_t bits = huffmanTable[i].bits;
    //     HuffmanNode *node = root;
    //     for (int b = bits - 1; b >= 0 ; b--) {
    //         // TODO investigate
    //         int bit = (code >> b) & 1;
    //         if (bit == 0) {
    //             if (!node -> left) {
    //                 node -> left = calloc(1, sizeof(HuffmanNode));
    //                 node -> left -> symbol = -1;
    //             }
    //             node = node -> left;
    //         } else {
    //             if (!node -> right) {
    //                 node -> right = calloc(1, sizeof(HuffmanNode));
    //                 node -> right -> symbol = -1;
    //             }
    //             node = node -> right;
    //         }
    //     }
    //     node -> symbol = i;
    // }
    // return root;
}

static HuffmanNode* getHuffmanNode(int index) {
    if (index >= 513) {
        // insufficient nodes
        return NULL;
    }
    return &HUFFMAN_NODES[index];
}

static char* huffmanDecodeTree(const uint8_t *src, size_t srcLen, HuffmanNode *root) {
    // TODO dst
    // * 8: 8 bits per byte
    char *dst = malloc(srcLen * 8 + 1);
    if (!dst) { return NULL; }
    size_t dstLen = 0;
    HuffmanNode *node = root;
    for (size_t i = 0; i < srcLen; i++) { // loop every byte
        for (int b = 7; b >= 0; b--) { // loop every bit in the byte, from the highest bit
            int bit = (src[i] >> b) & 1;
            node = bit ? node -> right : node -> left;
            if (!node) { goto done; } // error
            if (node -> symbol >= 0) {
                if (node -> symbol == 256) { goto done; } // EOS, decoding done
                dst[dstLen++] = (char) node -> symbol;
                node = root;
            }
        }
    }
done:
    dst[dstLen] = 0;
    return dst;
}

void freeHuffmanTreeFromRoot() {
    free(huffmanRoot);
}

static void freeHuffmanTree(HuffmanNode *node) {
    if (!node) { return; }
    freeHuffmanTree(node -> left);
    freeHuffmanTree(node -> right);
    free(node);
}

void writeHuffmanValue(unsigned char **ptr, const char *value, size_t valueLen) {
    if (valueLen == 0) {
        *(*ptr)++ = 0x80; // huffman encoded empty string
        return;
    }

    size_t maxHuffLen = valueLen * 2 + 10; // 1.5
    unsigned char *huffBuffer = malloc(maxHuffLen);
    if (!huffBuffer) { return; }

    size_t huffLen = hpackHuffmanEncode(value, valueLen, huffBuffer);

    // 7-bit prefix with Huffman flag 0x80
    hpackEncodeInteger(huffLen, 7, 0x80, ptr);

    memcpy(*ptr, huffBuffer, huffLen);
    (*ptr) += huffLen;

    free(huffBuffer);
}

void hpackEncodeInteger(size_t value, int prefixBits, unsigned char prefix, unsigned char **ptrPtr) {
    unsigned char *ptr = *ptrPtr;
    int maxPrefix = (1 << prefixBits) - 1;
    if (value < maxPrefix) {
        *ptr++ = prefix | value;
    } else {
        *ptr++ = prefix | maxPrefix;
        value -= maxPrefix;

        while (value >= 128) {
            *ptr++ = (value & 0x7F) | 0x80; // take the lower 7 bits, highest bit 1
            value >>= 7;
        }
        // last byte, highest bit 0
        *ptr++ = value & 0x7F;
    }

    *ptrPtr = ptr;
}

/**
 * TODO Explain
 * @param buf
 * @param pos
 * @param prefixSize
 * @param bufLen
 * @return
 */
size_t hpackDecodeInteger(uint8_t *buf, size_t *pos, uint8_t prefixSize, size_t bufLen) {
    if (*pos >= bufLen) return 0; // preventing index out of bound

    uint8_t mask = (1 << prefixSize) - 1;
    size_t value = buf[(*pos)++] & mask;
    if (value == mask) {
        uint8_t b;
        size_t shift = 0;

        do {
            if (*pos >= bufLen) return value;

            b = buf[(*pos)++];
            value += ((size_t) (b & 0x7F)) << shift;
            shift += 7;

            if (shift > 28) { // preventing Integer overflow
                return value;
            }
        } while (b & 0x80);
    }

    return value;
}

char* hpackDecodeString(unsigned char *payload, size_t *pos, size_t length) {
    if (!payload || !pos || *pos >= length) {
        return NULL;
    }

    size_t lenPos = *pos;
    size_t strLen = hpackDecodeInteger(payload, pos, 7, length);
    // 0x80 is to check the highest bit if huffman encoded
    uint8_t isHuffman = payload[lenPos] & 0x80;

    // Check bounds
    if (*pos + strLen > length) {
        return NULL;
    }

    char *result = NULL;
    if (isHuffman) {
        result = hpackHuffmanDecode(payload + *pos, strLen);
    } else {
        result = malloc(strLen + 1);
        if (result) {
            memcpy(result, payload + *pos, strLen);
            result[strLen] = '\0';
        }
    }

    *pos += strLen;
    return result;
}

DecompressedObj* decompress_GZip(unsigned char *combinedPayload, size_t combinedPayloadSize) {
    LOG("DEBUG", "Gzip, decompressing...\n");

    DecompressedObj *decompressedObj = malloc(sizeof(DecompressedObj));
    if (!decompressedObj) {
        LOG("ERROR", "(Gzip) Failed to allocate decompression object\n");
        return NULL;
    }
    decompressedObj -> error = ERR_NONE;
    decompressedObj -> decompressedPayload = NULL;

    // initialise zlib stream
    z_stream zs = { 0 };
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        LOG("ERROR", "(Gzip) Zlib Inflate failed\n");
        decompressedObj -> error = ERR_RESPONSE_GZIP_INFLATE_FAILED;
        return decompressedObj;
    }

    // allocate decompression buffer, assume 4x compression ratio
    size_t decompressedSize = combinedPayloadSize * 4;
    if (decompressedSize < 4096) { decompressedSize = 4096; }
    unsigned char *decompressed = malloc(decompressedSize);
    if (!decompressed) {
        inflateEnd(&zs);
        LOG("ERROR", "(Gzip) Failed to allocate decompression buffer\n");
        decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return decompressedObj;
    }

    zs.next_in = combinedPayload;
    zs.avail_in = combinedPayloadSize;
    zs.next_out = decompressed;
    zs.avail_out = decompressedSize;

    int status = 0;
    while (1) {
        status = inflate(&zs, Z_NO_FLUSH);
        if (status == Z_STREAM_END) break;
        if (status != Z_OK) {
            LOG("ERROR", "(Gzip) Zlib Inflate failed: %d\n", status);
            decompressedObj -> error = ERR_RESPONSE_GZIP_INFLATE_FAILED;
            free(decompressed);
            inflateEnd(&zs);
            return decompressedObj;
        }
        // need more output space
        size_t used = zs.next_out - decompressed;
        decompressedSize *= 2;
        unsigned char *newDecompressed = realloc(decompressed, decompressedSize);
        if (!newDecompressed) {
            LOG("ERROR", "(Gzip) Failed to reallocate decompression buffer\n");
            decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
            free(decompressed);
            inflateEnd(&zs);
            return decompressedObj;
        }

        decompressed = newDecompressed;
        zs.next_out = decompressed + used;
        zs.avail_out = decompressedSize - used;
    }
    size_t realSize = zs.next_out - decompressed;
    decompressed[realSize] = '\0';

    LOG("ERROR", "(Gzip) Decompressed content (%zu bytes): \n", realSize);

    decompressedObj -> decompressedPayload = decompressed;
    decompressedObj -> decompressedPayloadSize = realSize;

    // free(decompressed);

    // if (combinedPayload != NULL) {
    //     free(combinedPayload);
    // }
    inflateEnd(&zs);
    return decompressedObj;
}

DecompressedObj* decompress_Brotli(unsigned char *payload, size_t payloadSize) {
    LOG("DEBUG", "Brotli, decompressing...\n");

    DecompressedObj *decompressedObj = malloc(sizeof(DecompressedObj));
    if (!decompressedObj) {
        LOG("ERROR", "(Brotli) Failed to allocate decompression object\n");
        return NULL;
    }

    size_t decompressedSize = payloadSize * 8;
    if (decompressedSize < 4096) decompressedSize = 4096;
    unsigned char *decompressed = malloc(decompressedSize);
    if (!decompressed) {
        LOG("ERROR", "(Brotli) Failed to allocate decompression buffer\n");
        decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return decompressedObj;
    }
    const size_t availableIn = payloadSize;
    const uint8_t *nextIn = payload;
    size_t availableOut = decompressedSize;
    uint8_t *nextOut = decompressed;

    const BrotliDecoderResult result = BrotliDecoderDecompress(availableIn, nextIn, &availableOut, nextOut);
    if (result != BROTLI_DECODER_RESULT_SUCCESS) {
        free(decompressed);
        LOG("ERROR", "(Brotli) Inflate failed: %d\n", result);
        decompressedObj -> error = ERR_RESPONSE_BROTLI_INFLATE_FAILED;
        return decompressedObj;
    }
//    LOG("ERROR", "(Brotli) Decompressed content (%zu bytes):\n", availableOut);
//    LOG("ERROR", "%.*s\n", (int) availableOut, decompressed);
    decompressed[availableOut] = '\0';
    decompressedObj -> decompressedPayload = decompressed;
    decompressedObj -> decompressedPayloadSize = availableOut;
    // free(decompressed);
    // if (combinedPayload != NULL) {
    //     free(combinedPayload);
    // }
    return decompressedObj;
}

DecompressedObj* decompress_Deflate(unsigned char *combinedPayload, size_t combinedPayloadSize) {
    LOG("DEBUG", "Deflate, decompressing...\n");

    DecompressedObj *decompressedObj = malloc(sizeof(DecompressedObj));
    if (!decompressedObj) {
        LOG("ERROR", "(Brotli) Failed to allocate decompression object\n");
        return NULL;
    }
    decompressedObj -> error = ERR_NONE;
    decompressedObj -> decompressedPayload = NULL;
    decompressedObj -> decompressedPayloadSize = 0;

    // initialise zlib stream for deflate (raw deflate, no gzip/zlib headers)
    z_stream zs = { 0 };

    // Use -MAX_WBITS for raw deflate decoding (no header)
    // Use MAX_WBITS for zlib format (with header)
    // Try raw deflate first, if fails, try zlib format
    int windowBits = -MAX_WBITS;  // Raw deflate

    if (inflateInit2(&zs, windowBits) != Z_OK) {
        LOG("ERROR", "[ERROR] (Deflate) Zlib inflateInit2 failed\n");
        decompressedObj -> error = ERR_RESPONSE_DEFLATE_INFLATE_FAILED;
        return decompressedObj;
    }

    // Allocate initial decompression buffer
    size_t decompressedSize = combinedPayloadSize * 4;
    if (decompressedSize < 4096) { decompressedSize = 4096; }

    unsigned char *decompressed = malloc(decompressedSize);
    if (!decompressed) {
        inflateEnd(&zs);
        LOG("ERROR", "(Deflate) Failed to allocate decompression buffer\n");
        decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return decompressedObj;
    }

    zs.next_in = combinedPayload;
    zs.avail_in = combinedPayloadSize;
    zs.next_out = decompressed;
    zs.avail_out = decompressedSize;

    int status = 0;
    while (1) {
        status = inflate(&zs, Z_NO_FLUSH);

        if (status == Z_STREAM_END) {
            break;
        }

        if (status != Z_OK) {
            // If raw deflate fails, try with zlib header
            if (windowBits == -MAX_WBITS) {
                inflateEnd(&zs);
                free(decompressed);

                LOG("DEBUG", "(Deflate) Raw deflate failed, trying zlib format...\n");

                // Retry with zlib format (positive MAX_WBITS)
                windowBits = MAX_WBITS;
                if (inflateInit2(&zs, windowBits) != Z_OK) {
                    LOG("ERROR", "(Deflate) Zlib inflateInit2 (zlib format) failed\n");
                    decompressedObj -> error = ERR_RESPONSE_DEFLATE_INFLATE_FAILED;
                    return decompressedObj;
                }

                // Reallocate buffer
                decompressedSize = combinedPayloadSize * 4;
                if (decompressedSize < 4096) { decompressedSize = 4096; }
                decompressed = malloc(decompressedSize);
                if (!decompressed) {
                    inflateEnd(&zs);
                    LOG("ERROR", "(Deflate) Failed to reallocate decompression buffer\n");
                    decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
                    return decompressedObj;
                }

                zs.next_in = combinedPayload;
                zs.avail_in = combinedPayloadSize;
                zs.next_out = decompressed;
                zs.avail_out = decompressedSize;

                continue;  // Retry decompression
            }

            LOG("ERROR", "(Deflate) Zlib inflate failed: %d (%s)\n",
                status, zs.msg ? zs.msg : "unknown error");
            decompressedObj -> error = ERR_RESPONSE_DEFLATE_INFLATE_FAILED;
            free(decompressed);
            inflateEnd(&zs);
            return decompressedObj;
        }

        // Need more output space
        size_t used = zs.next_out - decompressed;
        decompressedSize *= 2;

        unsigned char *newDecompressed = realloc(decompressed, decompressedSize);
        if (!newDecompressed) {
            LOG("ERROR", "(Deflate) Failed to reallocate decompression buffer\n");
            decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
            free(decompressed);
            inflateEnd(&zs);
            return decompressedObj;
        }

        decompressed = newDecompressed;
        zs.next_out = decompressed + used;
        zs.avail_out = decompressedSize - used;
    }

    size_t realSize = zs.next_out - decompressed;
    decompressed[realSize] = '\0';

    LOG("DEBUG", "(Deflate) Decompressed %zu bytes to %zu bytes\n",
        combinedPayloadSize, realSize);

    decompressedObj -> decompressedPayload = decompressed;
    decompressedObj -> decompressedPayloadSize = realSize;

    inflateEnd(&zs);
    return decompressedObj;
}

DecompressedObj* decompress_Zstd(unsigned char *combinedPayload, size_t combinedPayloadSize) {
    LOG("DEBUG", "Zstd, decompressing...\n");

    DecompressedObj *decompressedObj = malloc(sizeof(DecompressedObj));
    if (!decompressedObj) {
        LOG("ERROR", "(Zstd) Failed to allocate decompression object\n");
        return NULL;
    }

    decompressedObj -> error = ERR_NONE;
    decompressedObj -> decompressedPayload = NULL;
    decompressedObj -> decompressedPayloadSize = 0;

    // Try to get decompressed size first
    unsigned long long const decompressedSize = ZSTD_getFrameContentSize(combinedPayload, combinedPayloadSize);

    unsigned char *decompressed = NULL;
    size_t result = 0;

    if (decompressedSize != ZSTD_CONTENTSIZE_UNKNOWN && decompressedSize != ZSTD_CONTENTSIZE_ERROR) {
        // Known size: use direct decompression
        LOG("ERROR", "(Zstd) Known decompressed size: %llu bytes\n", decompressedSize);

        if (decompressedSize > 100 * 1024 * 1024) {
            LOG("ERROR", "(Zstd) Decompressed size too large: %llu bytes\n", decompressedSize);
            decompressedObj -> error = ERR_RESPONSE_ZSTD_INFLATE_FAILED;
            return decompressedObj;
        }

        decompressed = malloc(decompressedSize + 1);
        if (!decompressed) {
            LOG("ERROR", "(Zstd) Failed to allocate decompression buffer\n");
            decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
            return decompressedObj;
        }

        result = ZSTD_decompress(decompressed, decompressedSize, combinedPayload, combinedPayloadSize);

        if (ZSTD_isError(result)) {
            LOG("ERROR", "(Zstd) Decompression failed: %s\n", ZSTD_getErrorName(result));
            free(decompressed);
            decompressedObj -> error = ERR_RESPONSE_ZSTD_INFLATE_FAILED;
            return decompressedObj;
        }
    } else {
        // Unknown size: use streaming decompression with dynamic buffer
        LOG("DEBUG", "(Zstd) Unknown decompressed size, using streaming mode\n");

        // Create decompression context
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        if (!dctx) {
            LOG("ERROR", "(Zstd) Failed to create decompression context\n");
            decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
            return decompressedObj;
        }

        // Start with initial buffer size
        size_t bufferSize = combinedPayloadSize * 4;
        if (bufferSize < 4096) bufferSize = 4096;

        decompressed = malloc(bufferSize);
        if (!decompressed) {
            ZSTD_freeDCtx(dctx);
            LOG("ERROR", "(Zstd) Failed to allocate initial buffer\n");
            decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
            return decompressedObj;
        }

        // Setup input/output buffers
        ZSTD_inBuffer input = { combinedPayload, combinedPayloadSize, 0 };
        ZSTD_outBuffer output = { decompressed, bufferSize, 0 };

        // Decompress in streaming mode
        while (1) {
            size_t const ret = ZSTD_decompressStream(dctx, &output, &input);

            if (ZSTD_isError(ret)) {
                LOG("ERROR", "(Zstd) Streaming decompression failed: %s\n", ZSTD_getErrorName(ret));
                free(decompressed);
                ZSTD_freeDCtx(dctx);
                decompressedObj -> error = ERR_RESPONSE_ZSTD_INFLATE_FAILED;
                return decompressedObj;
            }

            if (ret == 0) {
                // Decompression complete
                break;
            }

            // Need more output space
            if (output.pos >= output.size) {
                bufferSize *= 2;
                unsigned char *newBuffer = realloc(decompressed, bufferSize);
                if (!newBuffer) {
                    free(decompressed);
                    ZSTD_freeDCtx(dctx);
                    LOG("ERROR", "(Zstd) Failed to reallocate buffer\n");
                    decompressedObj -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
                    return decompressedObj;
                }

                decompressed = newBuffer;
                output.dst = decompressed;
                output.size = bufferSize;
            }
        }

        result = output.pos;
        ZSTD_freeDCtx(dctx);

        LOG("ERROR", "(Zstd) Streaming decompressed %zu bytes to %zu bytes\n",
            combinedPayloadSize, result);
    }

    // Null-terminate for safety
    decompressed[result] = '\0';

    decompressedObj -> decompressedPayload = decompressed;
    decompressedObj -> decompressedPayloadSize = result;

    return decompressedObj;
}

// base64 encode binary data
unsigned char* base64Encode(const unsigned char *data, size_t dataLen) {
    if (!data || dataLen == 0) return NULL;

    // Calculate output size: ceil(data_len / 3) * 4 + 1 for null terminator
    size_t output_len = ((dataLen + 2) / 3) * 4;
    unsigned char *output = malloc(output_len + 1);
    if (!output) return NULL;

    // BoringSSL's EVP_EncodeBlock always adds padding and returns unpadded length
    int encoded_len = EVP_EncodeBlock(output, data, dataLen);

    // EVP_EncodeBlock returns the length without padding, but writes with padding
    // The actual output is already null-terminated by EVP_EncodeBlock
    (void)encoded_len; // Suppress unused variable warning

    return output;
}