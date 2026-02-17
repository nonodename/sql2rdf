#include <iostream>
#include <serd/serd.h>

int main() {
    std::cout << "Hello SQL2RDF++" << std::endl;

    const char* test = "héllo"; // some utf8 text
    size_t nbytes = 0;
    SerdNodeFlags flags = 0;
    size_t nchars = serd_strlen((const uint8_t*)test, &nbytes, &flags);
    std::cout << "serd_strlen computed " << nchars << " characters (" << nbytes
              << " bytes)" << std::endl;

    // also demonstrate strerror helper
    const uint8_t* msg = serd_strerror(SERD_ERR_BAD_SYNTAX);
    std::cout << "serd_strerror(SERD_ERR_BAD_SYNTAX) = " << msg << std::endl;

    return 0;
}
