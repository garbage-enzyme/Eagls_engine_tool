#include "archive/index_cipher.hpp"

#include "support/msvc_rand.hpp"

namespace eagls::detail {

void apply_index_cipher(std::uint8_t* data, std::size_t size, std::uint32_t seed) noexcept {
    MsvcRand random{seed};
    for (std::size_t i = 0; i < size; ++i) {
        const auto key_position = static_cast<std::size_t>(random.next()) % index_key.size();
        data[i] ^= static_cast<std::uint8_t>(index_key[key_position]);
    }
}

}  // namespace eagls::detail
