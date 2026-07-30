#include "ktx.h"
#include "ktxint.h"
#include "texture1.h"

KTX_error_code
ktxTexture1_constructFromStreamAndHeader(ktxTexture1 *texture,
                                         ktxStream *stream,
                                         KTX_header *header,
                                         ktxTextureCreateFlags flags) {
    (void)texture;
    (void)stream;
    (void)header;
    (void)flags;
    return KTX_UNSUPPORTED_FEATURE;
}
