#include <zlib.h>

int main(void) {
    const char *version = zlibVersion();
    z_stream stream = {0};
    unsigned char window[32768];

    if (version[0] != '1') return 1;

    if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) return 2;
    if (deflateEnd(&stream) != Z_OK) return 3;
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 4;
    if (deflateEnd(&stream) != Z_OK) return 5;

    if (inflateInit(&stream) != Z_OK) return 6;
    if (inflateEnd(&stream) != Z_OK) return 7;
    if (inflateInit2(&stream, 15) != Z_OK) return 8;
    if (inflateEnd(&stream) != Z_OK) return 9;
    if (inflateBackInit(&stream, 15, window) != Z_OK) return 10;
    if (inflateBackEnd(&stream) != Z_OK) return 11;

    if (deflateInit_(&stream, Z_DEFAULT_COMPRESSION, version,
                     sizeof(stream)) != Z_OK) return 12;
    if (deflateEnd(&stream) != Z_OK) return 13;
    if (inflateInit_(&stream, "0", sizeof(stream)) != Z_VERSION_ERROR) return 14;

    // An uninitialized declaration takes a separate default-initializer path
    // in Tenjin. Initialize the allocation hooks as required by zlib.
    z_stream implicit_stream;
    implicit_stream.zalloc = Z_NULL;
    implicit_stream.zfree = Z_NULL;
    implicit_stream.opaque = Z_NULL;
    if (inflateInit2(&implicit_stream, -15) != Z_OK) return 15;
    if (inflateEnd(&implicit_stream) != Z_OK) return 16;
    return 0;
}
