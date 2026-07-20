#include "RuntimeControlProtocol.h"

#include <stdexcept>

namespace vkr::control {

bool isValidRuntimePipeSuffix(std::string_view suffix) {
    if (suffix.size() > kMaxPipeSuffixLength)
        return false;
    for (const unsigned char character : suffix) {
        const bool alpha =
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        if (!alpha && !digit && character != '-' && character != '_')
            return false;
    }
    return true;
}

RuntimeControlEndpoint makeRuntimeControlEndpoint(std::string_view suffix) {
    if (!isValidRuntimePipeSuffix(suffix)) {
        throw std::invalid_argument(
            "runtime control pipe suffix must contain at most " +
            std::to_string(kMaxPipeSuffixLength) +
            " ASCII letters, digits, '-' or '_'");
    }

    RuntimeControlEndpoint endpoint;
    endpoint.suffix = suffix;
    endpoint.nameUtf8 = kPipeNameUtf8;
    endpoint.name = kPipeName;
    if (!suffix.empty()) {
        endpoint.nameUtf8.push_back('.');
        endpoint.nameUtf8.append(suffix);
        endpoint.name.push_back(L'.');
        endpoint.name.append(suffix.begin(), suffix.end());
    }
    return endpoint;
}

} // namespace vkr::control
