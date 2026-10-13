#include <eagls/version.hpp>
#include <eagls/version_config.hpp>

namespace eagls {

std::string_view version() noexcept {
    return detail::build_version;
}

}
